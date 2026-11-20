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

#include "db/version_edit.h"

#include "util/coding.h"
#include <string>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Appends a varint32 length followed by the bytes of `value`.
Status put_length_prefixed(std::string *dst, std::string_view value) {
    if (value.size() > UINT32_MAX) {
        return Status::invalid_argument("version edit: field exceeds 32-bit length", std::to_string(value.size()));
    }
    put_varint_32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
    return Status::ok();
}

bool read_varint_32(std::string_view *input, uint32_t *value) { return get_varint_32(input, value); }

bool read_varint_64(std::string_view *input, uint64_t *value) { return get_varint_64(input, value); }

// Reads a varint32 length followed by that many bytes into *out.
bool read_length_prefixed(std::string_view *input, std::string *out) {
    uint32_t length = 0;
    if (!read_varint_32(input, &length)) { return false; }
    if (length > input->size()) { return false; }
    out->assign(input->data(), length);
    input->remove_prefix(length);
    return true;
}

bool read_fixed_32(std::string_view *input, uint32_t *value) {
    if (input->size() < sizeof(uint32_t)) { return false; }
    *value = decode_fixed_32(input->data());
    input->remove_prefix(sizeof(uint32_t));
    return true;
}

bool read_fixed_64(std::string_view *input, uint64_t *value) {
    if (input->size() < sizeof(uint64_t)) { return false; }
    *value = decode_fixed_64(input->data());
    input->remove_prefix(sizeof(uint64_t));
    return true;
}

Status corruption(const char *what) { return Status::corruption(std::string("version edit: ") + what); }

} // namespace

void VersionEdit::clear() { *this = VersionEdit(); }

void VersionEdit::set_comparator_name(std::string_view name) {
    comparator_name_.assign(name.data(), name.size());
    has_comparator_name_ = true;
}

void VersionEdit::set_log_number(uint64_t log_number) {
    log_number_ = log_number;
    has_log_number_ = true;
}

void VersionEdit::set_next_file_number(uint64_t next_file_number) {
    next_file_number_ = next_file_number;
    has_next_file_number_ = true;
}

void VersionEdit::set_last_timestamp(uint64_t timestamp) {
    last_timestamp_ = timestamp;
    has_last_timestamp_ = true;
}

void VersionEdit::add_compact_pointer(uint32_t level, std::string_view key) {
    compact_pointers_.push_back(CompactPointer{level, std::string(key)});
}

void VersionEdit::delete_table(uint32_t level, uint64_t table_id) {
    deleted_tables_.push_back(DeletedTable{level, table_id});
}

void VersionEdit::add_table(uint32_t level, const FileMetaData &meta) {
    added_tables_.push_back(AddedTable{level, meta});
}

void VersionEdit::set_vlog_head(uint32_t file_id, uint64_t offset) {
    vlog_head_ = VlogPosition{file_id, offset};
    has_vlog_head_ = true;
}

void VersionEdit::set_vlog_tail(uint32_t file_id, uint64_t offset) {
    vlog_tail_ = VlogPosition{file_id, offset};
    has_vlog_tail_ = true;
}

void VersionEdit::add_vlog_segment_stat(uint32_t file_id, uint64_t total_size, uint64_t stale_bytes) {
    vlog_segment_stats_.push_back(VlogSegmentStat{file_id, total_size, stale_bytes});
}

void VersionEdit::delete_vlog_segment(uint32_t file_id) { deleted_vlog_segments_.push_back(file_id); }

Status VersionEdit::encode(std::string *payload) const {
    if (payload == nullptr) { return Status::invalid_argument("version edit: null payload"); }
    payload->clear();

    if (has_comparator_name_) {
        payload->push_back(static_cast<char>(VersionEditTag::kComparator));
        Status s = put_length_prefixed(payload, comparator_name_);
        if (!s.is_ok()) { return s; }
    }
    if (has_log_number_) {
        payload->push_back(static_cast<char>(VersionEditTag::kLogNumber));
        put_varint_64(payload, log_number_);
    }
    if (has_next_file_number_) {
        payload->push_back(static_cast<char>(VersionEditTag::kNextFileNumber));
        put_varint_64(payload, next_file_number_);
    }
    if (has_last_timestamp_) {
        payload->push_back(static_cast<char>(VersionEditTag::kLastTimestamp));
        put_fixed_64(payload, last_timestamp_);
    }

    for (const CompactPointer &pointer : compact_pointers_) {
        payload->push_back(static_cast<char>(VersionEditTag::kCompactPointer));
        put_varint_32(payload, pointer.level);
        Status s = put_length_prefixed(payload, pointer.key);
        if (!s.is_ok()) { return s; }
    }

    for (const DeletedTable &table : deleted_tables_) {
        payload->push_back(static_cast<char>(VersionEditTag::kDeletedTable));
        put_varint_32(payload, table.level);
        put_varint_64(payload, table.table_id);
    }

    for (const AddedTable &table : added_tables_) {
        const FileMetaData &meta = table.meta;
        payload->push_back(static_cast<char>(VersionEditTag::kAddedTable));
        put_varint_32(payload, table.level);
        put_varint_64(payload, meta.table_id);
        Status s = put_length_prefixed(payload, meta.smallest_key);
        if (!s.is_ok()) { return s; }
        s = put_length_prefixed(payload, meta.largest_key);
        if (!s.is_ok()) { return s; }
        put_fixed_64(payload, meta.min_ts);
        put_fixed_64(payload, meta.max_ts);
        put_varint_64(payload, meta.data_size);
        put_varint_64(payload, meta.index_size);
        put_varint_64(payload, meta.summary_size);
        put_varint_64(payload, meta.filter_size);
        put_varint_64(payload, meta.entry_count);
    }

    if (has_vlog_head_) {
        payload->push_back(static_cast<char>(VersionEditTag::kVlogHead));
        put_fixed_32(payload, vlog_head_.file_id);
        put_fixed_64(payload, vlog_head_.offset);
    }
    if (has_vlog_tail_) {
        payload->push_back(static_cast<char>(VersionEditTag::kVlogTail));
        put_fixed_32(payload, vlog_tail_.file_id);
        put_fixed_64(payload, vlog_tail_.offset);
    }

    for (const VlogSegmentStat &stat : vlog_segment_stats_) {
        payload->push_back(static_cast<char>(VersionEditTag::kVlogSegmentStat));
        put_fixed_32(payload, stat.file_id);
        put_fixed_64(payload, stat.total_size);
        put_fixed_64(payload, stat.stale_bytes);
    }

    for (uint32_t file_id : deleted_vlog_segments_) {
        payload->push_back(static_cast<char>(VersionEditTag::kDeletedVlogSegment));
        put_fixed_32(payload, file_id);
    }

    return Status::ok();
}

Status VersionEdit::decode(std::string_view payload) {
    clear();

    while (!payload.empty()) {
        const auto tag = static_cast<VersionEditTag>(static_cast<uint8_t>(payload.front()));
        payload.remove_prefix(1);

        switch (tag) {
        case VersionEditTag::kComparator: {
            if (!read_length_prefixed(&payload, &comparator_name_)) { return corruption("truncated comparator"); }
            has_comparator_name_ = true;
            break;
        }
        case VersionEditTag::kLogNumber: {
            if (!read_varint_64(&payload, &log_number_)) { return corruption("truncated log number"); }
            has_log_number_ = true;
            break;
        }
        case VersionEditTag::kNextFileNumber: {
            if (!read_varint_64(&payload, &next_file_number_)) { return corruption("truncated next file number"); }
            has_next_file_number_ = true;
            break;
        }
        case VersionEditTag::kLastTimestamp: {
            if (!read_fixed_64(&payload, &last_timestamp_)) { return corruption("truncated last timestamp"); }
            has_last_timestamp_ = true;
            break;
        }
        case VersionEditTag::kCompactPointer: {
            CompactPointer pointer;
            if (!read_varint_32(&payload, &pointer.level) || !read_length_prefixed(&payload, &pointer.key)) {
                return corruption("truncated compact pointer");
            }
            compact_pointers_.push_back(std::move(pointer));
            break;
        }
        case VersionEditTag::kDeletedTable: {
            DeletedTable table;
            if (!read_varint_32(&payload, &table.level) || !read_varint_64(&payload, &table.table_id)) {
                return corruption("truncated deleted table");
            }
            deleted_tables_.push_back(table);
            break;
        }
        case VersionEditTag::kAddedTable: {
            AddedTable table;
            FileMetaData &meta = table.meta;
            if (!read_varint_32(&payload, &table.level) || !read_varint_64(&payload, &meta.table_id) ||
                !read_length_prefixed(&payload, &meta.smallest_key) ||
                !read_length_prefixed(&payload, &meta.largest_key) || !read_fixed_64(&payload, &meta.min_ts) ||
                !read_fixed_64(&payload, &meta.max_ts) || !read_varint_64(&payload, &meta.data_size) ||
                !read_varint_64(&payload, &meta.index_size) || !read_varint_64(&payload, &meta.summary_size) ||
                !read_varint_64(&payload, &meta.filter_size) || !read_varint_64(&payload, &meta.entry_count)) {
                return corruption("truncated added table");
            }
            added_tables_.push_back(std::move(table));
            break;
        }
        case VersionEditTag::kVlogHead: {
            if (!read_fixed_32(&payload, &vlog_head_.file_id) || !read_fixed_64(&payload, &vlog_head_.offset)) {
                return corruption("truncated vLog head");
            }
            has_vlog_head_ = true;
            break;
        }
        case VersionEditTag::kVlogTail: {
            if (!read_fixed_32(&payload, &vlog_tail_.file_id) || !read_fixed_64(&payload, &vlog_tail_.offset)) {
                return corruption("truncated vLog tail");
            }
            has_vlog_tail_ = true;
            break;
        }
        case VersionEditTag::kVlogSegmentStat: {
            VlogSegmentStat stat;
            if (!read_fixed_32(&payload, &stat.file_id) || !read_fixed_64(&payload, &stat.total_size) ||
                !read_fixed_64(&payload, &stat.stale_bytes)) {
                return corruption("truncated vLog segment stat");
            }
            vlog_segment_stats_.push_back(stat);
            break;
        }
        case VersionEditTag::kDeletedVlogSegment: {
            uint32_t file_id = 0;
            if (!read_fixed_32(&payload, &file_id)) { return corruption("truncated deleted vLog segment"); }
            deleted_vlog_segments_.push_back(file_id);
            break;
        }
        default: return Status::corruption("version edit: unknown tag", std::to_string(static_cast<uint8_t>(tag)));
        }
    }

    return Status::ok();
}

Status VersionEdit::append_to(LogWriter *writer) const {
    if (writer == nullptr) { return Status::invalid_argument("version edit: null writer"); }

    std::string payload;
    Status s = encode(&payload);
    if (!s.is_ok()) { return s; }
    return writer->append(payload);
}

Status VersionEdit::read_from(LogReader *reader) {
    if (reader == nullptr) { return Status::invalid_argument("version edit: null reader"); }

    std::string_view record;
    Status s = reader->next(&record);
    if (!s.is_ok()) { return s; }
    return decode(record);
}

} // namespace ANCHORDB_NAMESPACE
