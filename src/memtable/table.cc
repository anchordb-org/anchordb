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
#include "sstable/record_buffer_reader.h"
#include "sstable/record_writer.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Entry layout:
// clang-format off
/*
+----------------------------+----------------------------------------+---------------------+-------------+
| internal_key_size (Varint) | internal_key_bytes [user_key, tag(8B)] | value_size (Varint) | value_bytes |
+----------------------------+----------------------------------------+---------------------+-------------+
*/
// clang-format on
// tag = (sequence << 8) | type; 56Bit -> sequence, 8Bit -> type(EntryType)

std::string_view MemtableIterator::value() const {
    status_ = Status::ok();

    // Internal key, then the length-prefixed value bytes.
    std::string_view key_slice = get_len_prefixed_slice(iter_.key());
    const char *value_ptr = key_slice.data() + key_slice.size();
    const uint64_t tag =
        decode_fixed_64(key_slice.data() + key_slice.size() - 8);

    if (static_cast<EntryType>(tag & 0xff) != EntryType::Blob) {
        return get_len_prefixed_slice(value_ptr);
    }

    std::string_view encoded = get_len_prefixed_slice(value_ptr);
    BlobHandle handle;
    if (!parse_blob_handle(encoded, &handle)) {
        status_ = Status::corruption("memtable: invalid blob handle");
        return std::string_view();
    }
    if (memtable_ == nullptr) {
        status_ = Status::not_supported("memtable: blob value has no memtable");
        return std::string_view();
    }

    std::string_view user_key(key_slice.data(), key_slice.size() - 8);
    status_ = memtable_->read_blob_(handle, user_key, &value_scratch_);
    if (!status_.is_ok()) { return std::string_view(); }
    return value_scratch_;
}

Status MemTable::add(seq_num seq, EntryType type, std::string_view key,
                     std::string_view value) {
    if (key.size() > max_key_size_) {
        return Status::invalid_argument("memtable: key too large");
    }

    // Large inline values live in the value log.  The memtable entry keeps a
    // BlobHandle (file id, record size, record offset) as its value bytes and
    // is tagged EntryType::blob.  An entry already tagged blob carries a handle
    // supplied by the caller and is stored unchanged.
    EntryType stored_type = type;
    std::string_view stored_value = value;
    std::string blob_handle;
    if (vlog_writer_ != nullptr && type == EntryType::Inline &&
        value.size() > value_threshold_) {
        const uint64_t timestamp = env_ != nullptr ? env_->now_micros() : 0;
        DataRecordLocation location;
        Status s = vlog_writer_->add(key, value, EntryType::Blob, timestamp,
                                     &location);
        if (!s.is_ok()) { return s; }
        // Make the record visible to a reader that opens the file separately.
        s = vlog_writer_->flush();
        if (!s.is_ok()) { return s; }

        BlobHandle handle;
        handle.file_id = vlog_file_id_;
        handle.size = location.size;
        handle.offset = location.offset;
        append_blob_handle(&blob_handle, handle);
        stored_type = EntryType::Blob;
        stored_value = blob_handle;
    }

    size_t key_size = key.size();
    size_t value_size = stored_value.size();
    // Both lengths are stored as varint32; reject entries that would truncate.
    if (key_size > UINT32_MAX - 8 || value_size > UINT32_MAX) {
        return Status::invalid_argument("memtable: entry too large");
    }
    size_t internal_key_size = key_size + 8;
    const size_t encoded_len = varint_len(key_size) + internal_key_size +
                               varint_len(value_size) + value_size;
    char *buf = arena_.allocate(encoded_len);
    char *p = encode_varint_32(buf, internal_key_size);
    if (key_size != 0) { std::memcpy(p, key.data(), key_size); }
    p += key_size;
    encode_fixed_64(p, (seq << 8) | static_cast<uint64_t>(stored_type));
    p += 8;
    p = encode_varint_32(p, value_size);
    if (value_size != 0) { std::memcpy(p, stored_value.data(), value_size); }
    assert(p + value_size == buf + encoded_len);
    table_.insert(buf);
    return Status::ok();
}

bool MemTable::get(const LookUpKey &key, std::string *value, Status *s) {
    std::string_view mem_key = key.memtable_key();
    typename Table::Iterator iter(&table_);
    iter.seek(mem_key.data());
    if (iter.valid()) {
        const char *entry = iter.key();
        uint32_t key_len;
        const char *key_ptr = get_varint_32_ptr(entry, entry + 5, &key_len);
        if (comparator_.compare(std::string_view(key_ptr, key_len - 8),
                                key.user_key()) == 0) {
            const uint64_t tag = decode_fixed_64(key_ptr + key_len - 8);
            switch (static_cast<EntryType>(tag & 0xff)) {
            case EntryType::Inline: {
                std::string_view v = get_len_prefixed_slice(key_ptr + key_len);
                value->assign(v.data(), v.size());
                return true;
            }
            case EntryType::Tombstone: {
                *s = Status::not_found(std::string_view());
                return false;
            }
            case EntryType::Blob: {
                std::string_view encoded =
                    get_len_prefixed_slice(key_ptr + key_len);
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
        }
    }
    return false;
}

Status MemTable::read_blob_(const BlobHandle &handle,
                            std::string_view expected_key,
                            std::string *value) const {
    if (value == nullptr) {
        return Status::invalid_argument("memtable: null value");
    }
    if (env_ == nullptr) {
        return Status::not_supported("memtable: no env for blob read");
    }
    if (handle.size < kDataRecordHeaderSize) {
        return Status::corruption("memtable: vlog record size too small",
                                  std::to_string(handle.size));
    }

    const std::string fname = vlog_filename(vlog_dbname_, handle.file_id);
    uint64_t file_size = 0;
    Status s = env_->get_file_size(fname, &file_size);
    if (!s.is_ok()) { return s; }
    if (handle.offset > file_size || handle.size > file_size - handle.offset) {
        return Status::corruption("memtable: vlog record out of range",
                                  std::to_string(handle.offset));
    }

    RandomAccessFileReader *reader = nullptr;
    s = env_->new_random_access_file_reader(fname, &reader);
    if (!s.is_ok()) { return s; }

    std::string record_bytes;
    record_bytes.resize(handle.size);
    std::string_view view;
    s = reader->read(handle.offset, handle.size, &view, record_bytes.data());
    delete reader;
    if (!s.is_ok()) { return s; }
    if (view.size() != handle.size) {
        return Status::corruption("memtable: vlog record short read",
                                  std::to_string(view.size()));
    }

    DataRecordBufferReader record_reader(view);
    ParsedDataRecord record;
    bool done = false;
    s = record_reader.next(&record, &done);
    if (!s.is_ok()) { return s; }
    if (done) { return Status::corruption("memtable: vlog record missing"); }
    if (!record_reader.done()) {
        return Status::corruption("memtable: vlog record has trailing bytes");
    }
    if (!record.is_blob()) {
        return Status::corruption("memtable: vlog record is not a blob");
    }
    if (record.key != expected_key) {
        return Status::corruption("memtable: vlog record key mismatch");
    }

    value->assign(record.value.data(), record.value.size());
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
