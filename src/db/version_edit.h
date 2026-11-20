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
#include "db/file_metadata.h"
#include "db/log.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ANCHORDB_NAMESPACE {

// Tag ids for the entries that make up one VersionEdit payload.  The payload is
// a concatenation of `[tag (1B) + fields]` entries; the enclosing LogWriter
// record supplies the checksum and length frame.  The numeric values are part
// of the on-disk format and must not be renumbered.
enum class VersionEditTag : uint8_t {
    kComparator = 0x01,
    kLogNumber = 0x02,
    kNextFileNumber = 0x03,
    kLastTimestamp = 0x04,
    kCompactPointer = 0x05,
    kDeletedTable = 0x06,
    kAddedTable = 0x07,
    kVlogHead = 0x08,
    kVlogTail = 0x09,
    kVlogSegmentStat = 0x0A,
    kDeletedVlogSegment = 0x0B,
};

// One `kCompactPointer` entry: the next start key for round-robin compaction at
// `level`.
struct CompactPointer {
    uint32_t level = 0;
    std::string key;
};

// One `kDeletedTable` entry: `table_id` (and its four split files) is obsolete
// in `level`.
struct DeletedTable {
    uint32_t level = 0;
    uint64_t table_id = 0;
};

// One `kAddedTable` entry: atomically commit `meta`'s four-file SSTable into
// `level`.
struct AddedTable {
    uint32_t level = 0;
    FileMetaData meta;
};

// A committed position in a vLog file.  Used by both `kVlogHead` and
// `kVlogTail`.
struct VlogPosition {
    uint32_t file_id = 0;
    uint64_t offset = 0;
};

// One `kVlogSegmentStat` entry: byte accounting for vLog `file_id`.
struct VlogSegmentStat {
    uint32_t file_id = 0;
    uint64_t total_size = 0;
    uint64_t stale_bytes = 0;
};

// A single logical edit to the Version.  Each MANIFEST record is one encoded
// VersionEdit (see the tag registry above).
//
// Encoding appends the entries in a stable order: the scalar fields first, then
// compact pointers, deleted tables, added tables, and the vLog entries.  Only
// fields that have been set (or added) are emitted, so an edit carries exactly
// the changes it represents.  Decoding accepts any order of tags and rejects
// unknown tags, truncated fields and trailing garbage as corruption.
//
// The same inner encoding is used on the wire; log framing, checksums and crash
// tail handling come from LogWriter/LogReader.
class VersionEdit {
  public:
    VersionEdit() = default;

    void clear();

    // ---- Scalar fields ---------------------------------------------------
    void set_comparator_name(std::string_view name);
    void set_log_number(uint64_t log_number);
    void set_next_file_number(uint64_t next_file_number);
    void set_last_timestamp(uint64_t timestamp);

    bool has_comparator_name() const { return has_comparator_name_; }
    bool has_log_number() const { return has_log_number_; }
    bool has_next_file_number() const { return has_next_file_number_; }
    bool has_last_timestamp() const { return has_last_timestamp_; }

    const std::string &comparator_name() const { return comparator_name_; }
    uint64_t log_number() const { return log_number_; }
    uint64_t next_file_number() const { return next_file_number_; }
    uint64_t last_timestamp() const { return last_timestamp_; }

    // ---- Leveled compaction state ---------------------------------------
    void add_compact_pointer(uint32_t level, std::string_view key);
    void delete_table(uint32_t level, uint64_t table_id);
    void add_table(uint32_t level, const FileMetaData &meta);

    const std::vector<CompactPointer> &compact_pointers() const { return compact_pointers_; }
    const std::vector<DeletedTable> &deleted_tables() const { return deleted_tables_; }
    const std::vector<AddedTable> &added_tables() const { return added_tables_; }

    // ---- vLog state ------------------------------------------------------
    void set_vlog_head(uint32_t file_id, uint64_t offset);
    void set_vlog_tail(uint32_t file_id, uint64_t offset);
    void add_vlog_segment_stat(uint32_t file_id, uint64_t total_size, uint64_t stale_bytes);
    void delete_vlog_segment(uint32_t file_id);

    bool has_vlog_head() const { return has_vlog_head_; }
    bool has_vlog_tail() const { return has_vlog_tail_; }

    const VlogPosition &vlog_head() const { return vlog_head_; }
    const VlogPosition &vlog_tail() const { return vlog_tail_; }
    const std::vector<VlogSegmentStat> &vlog_segment_stats() const { return vlog_segment_stats_; }
    const std::vector<uint32_t> &deleted_vlog_segments() const { return deleted_vlog_segments_; }

    // Serializes the edit into *payload (replacing its contents).  The result is
    // exactly the inner payload of one MANIFEST record; the caller frames it
    // with LogWriter::append()/append_to().
    Status encode(std::string *payload) const;

    // Decodes one inner payload, replacing this edit's contents.  `payload` is
    // exactly the bytes returned by LogReader::next(); the edit owns copies of
    // every string it decodes.
    Status decode(std::string_view payload);

    // Encodes this edit and appends it to `writer` as one log record.
    Status append_to(LogWriter *writer) const;

    // Reads the next record from `reader` and decodes it into this edit.
    // A crash-truncated tail is surfaced as the reader's corruption status; the
    // recovery layer decides whether to truncate there.
    Status read_from(LogReader *reader);

  private:
    bool has_comparator_name_ = false;
    bool has_log_number_ = false;
    bool has_next_file_number_ = false;
    bool has_last_timestamp_ = false;
    bool has_vlog_head_ = false;
    bool has_vlog_tail_ = false;

    std::string comparator_name_;
    uint64_t log_number_ = 0;
    uint64_t next_file_number_ = 0;
    uint64_t last_timestamp_ = 0;

    std::vector<CompactPointer> compact_pointers_;
    std::vector<DeletedTable> deleted_tables_;
    std::vector<AddedTable> added_tables_;

    VlogPosition vlog_head_;
    VlogPosition vlog_tail_;
    std::vector<VlogSegmentStat> vlog_segment_stats_;
    std::vector<uint32_t> deleted_vlog_segments_;
};

} // namespace ANCHORDB_NAMESPACE
