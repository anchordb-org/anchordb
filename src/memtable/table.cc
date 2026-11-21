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

#include "anchordb/env.h"
#include "file/filename.h"
#include "sstable/data_record.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

std::string_view MemtableIterator::value() const {
    status_ = Status::ok();

    MemtableEntry entry;
    const char *value_field = MemtableEntry::decode_key(iter_.key(), &entry);
    if (value_field == nullptr) {
        status_ = Status::corruption("memtable: invalid entry key");
        return std::string_view();
    }

    if (entry.type() != EntryType::Blob) { return MemtableEntry::decode_value(value_field); }

    std::string_view encoded = MemtableEntry::decode_value(value_field);
    BlobHandle handle;
    if (!parse_blob_handle(encoded, &handle)) {
        status_ = Status::corruption("memtable: invalid blob handle");
        return std::string_view();
    }
    if (memtable_ == nullptr) {
        status_ = Status::not_supported("memtable: blob value has no memtable");
        return std::string_view();
    }

    status_ = memtable_->read_blob_(handle, entry.key(), &value_scratch_);
    if (!status_.is_ok()) { return std::string_view(); }
    return value_scratch_;
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
    if (key.size() > max_key_size_) { return Status::invalid_argument("memtable: key too large"); }

    // Large inline values live in the value log.  The memtable entry keeps a
    // BlobHandle (file id, record size, record offset) as its value bytes and
    // is tagged EntryType::blob.  An entry already tagged blob carries a handle
    // supplied by the caller and is stored unchanged.
    EntryType stored_type = type;
    std::string_view stored_value = value;
    std::string blob_handle;
    if (vlog_writer_ != nullptr && type == EntryType::Inline && value.size() > value_threshold_) {
        DataRecordLocation location;
        Status s = vlog_writer_->add(key, value, EntryType::Blob, timestamp, &location);
        if (!s.is_ok()) { return s; }
        // Make the record durable before the WAL records a handle to it.
        s = vlog_writer_->sync();
        if (!s.is_ok()) { return s; }

        BlobHandle handle;
        handle.file_id = vlog_file_id_;
        handle.size = location.size;
        handle.offset = location.offset;
        append_blob_handle(&blob_handle, handle);
        stored_type = EntryType::Blob;
        stored_value = blob_handle;
    }

    // Both lengths are stored as varint32; reject entries that would truncate.
    if (key.size() > UINT32_MAX || stored_value.size() > UINT32_MAX) {
        return Status::invalid_argument("memtable: entry too large");
    }
    const MemtableEntry entry(key, timestamp, stored_type, stored_value);
    const size_t encoded_len = entry.encoded_size();
    char *buf = arena_.allocate(encoded_len);
    char *end = entry.encode(buf);
    assert(end == buf + encoded_len);
    table_.insert(buf);
    return Status::ok();
}

bool MemTable::get(const LookUpKey &key, std::string *value, Status *s) {
    typename Table::Iterator iter(&table_);
    iter.seek(key.memtable_key().data());
    if (!iter.valid()) { return false; }

    MemtableEntry entry;
    const char *value_field = MemtableEntry::decode_key(iter.key(), &entry);
    if (value_field == nullptr) {
        *s = Status::corruption("memtable: invalid entry key");
        return false;
    }
    if (comparator_.compare(entry.key(), key.user_key()) != 0) { return false; }

    switch (entry.type()) {
    case EntryType::Inline: {
        std::string_view v = MemtableEntry::decode_value(value_field);
        value->assign(v.data(), v.size());
        return true;
    }
    case EntryType::Tombstone: {
        *s = Status::not_found(std::string_view());
        return false;
    }
    case EntryType::Blob: {
        std::string_view encoded = MemtableEntry::decode_value(value_field);
        BlobHandle handle;
        if (!parse_blob_handle(encoded, &handle)) {
            *s = Status::corruption("memtable: invalid blob handle");
            return false;
        }
        Status read_status = read_blob_(handle, key.user_key(), value);
        if (!read_status.is_ok()) {
            *s = read_status;
            return false;
        }
        return true;
    }
    }

    return false;
}

Status MemTable::read_blob_(const BlobHandle &handle, std::string_view expected_key, std::string *value) const {
    if (value == nullptr) { return Status::invalid_argument("memtable: null value"); }
    if (env_ == nullptr) { return Status::not_supported("memtable: no env for blob read"); }
    if (vlog_reader_cache_ == nullptr) { return Status::not_supported("memtable: no vlog reader cache"); }
    if (handle.size < kDataRecordHeaderSize) {
        return Status::corruption("memtable: vlog record size too small", std::to_string(handle.size));
    }

    const std::string fname = vlog_filename(vlog_dbname_, handle.file_id);
    uint64_t file_size = 0;
    Status s = env_->get_file_size(fname, &file_size);
    if (!s.is_ok()) { return s; }
    if (handle.offset > file_size || handle.size > file_size - handle.offset) {
        return Status::corruption("memtable: vlog record out of range", std::to_string(handle.offset));
    }

    std::string record_bytes;
    record_bytes.resize(handle.size);
    std::string_view view;
    s = vlog_reader_cache_->read(fname, handle.offset, handle.size, &view, record_bytes.data());
    if (!s.is_ok()) { return s; }
    if (view.size() != handle.size) {
        return Status::corruption("memtable: vlog record short read", std::to_string(view.size()));
    }

    DataRecordBufferReader record_reader(view);
    ParsedDataRecord record;
    bool done = false;
    s = record_reader.next(&record, &done);
    if (!s.is_ok()) { return s; }
    if (done) { return Status::corruption("memtable: vlog record missing"); }
    if (!record_reader.done()) { return Status::corruption("memtable: vlog record has trailing bytes"); }
    if (!record.is_blob()) { return Status::corruption("memtable: vlog record is not a blob"); }
    if (record.key != expected_key) { return Status::corruption("memtable: vlog record key mismatch"); }

    value->assign(record.value.data(), record.value.size());
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
