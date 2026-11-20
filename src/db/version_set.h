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
#include "anchordb/comparator.h"
#include "anchordb/status.h"
#include "db/file_metadata.h"
#include "db/version.h"
#include "db/version_edit.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ANCHORDB_NAMESPACE {

class Env;
class ManifestFile;

// Owns the durable version metadata, the current immutable Version, and the
// live-file bookkeeping that flush/compaction/vLog GC depend on.
//
// The VersionSet is the authority for:
//   * which MANIFEST is live (via CURRENT) and which WAL is active,
//   * the current Version and every older Version still pinned by a reader,
//   * the next unused file number,
//   * the highest committed timestamp,
//   * round-robin compaction pointers,
//   * committed vLog head/tail and per-segment statistics, and
//   * files that became obsolete and can be deleted once no Version pins them.
//
// recover() reads CURRENT, replays every VersionEdit in that MANIFEST through
// ManifestReader, and rebuilds the state above.  A cleanly terminated MANIFEST
// is reopened for appending; a torn tail (the crash case) is truncated in place
// to the last complete record and then reopened for appending.  If the Env
// cannot truncate, recovery instead writes a fresh MANIFEST snapshot and
// atomically repoints CURRENT.
//
// log_and_apply() appends one VersionEdit record, syncs it, then installs a new
// immutable Version.  The old Version is unref'd, not mutated, so any reader
// holding a reference keeps its files alive.  Files no longer referenced by any
// live Version are reported through get_obsolete_files(); get_live_files()
// reports the complete live set for orphan cleanup during recovery.
//
// Not thread-safe: the DB layer serializes VersionSet mutations.  Versions may
// be ref'd/unref'd by readers, but ref/unref must still be paired with the
// DB-level lock when they can race with an install.
class VersionSet {
  public:
    // `env`, `comparator`, `num_levels`, `manifest_max_bytes` and `dbname` are
    // captured at construction; the Env is borrowed and must outlive this
    // object.  A `manifest_max_bytes` of 0 disables automatic MANIFEST
    // rotation.  The DB/orchestrator layer derives these values from Options.
    VersionSet(Env *env, const Comparator &comparator, std::string dbname, size_t num_levels,
               uint64_t manifest_max_bytes);

    VersionSet(const VersionSet &) = delete;
    VersionSet &operator=(const VersionSet &) = delete;

    ~VersionSet();

    // Reads CURRENT, replays the live MANIFEST, and prepares a writer for later
    // edits.  Returns NotFound when CURRENT does not exist (a new database);
    // corruption when CURRENT names something other than a MANIFEST or the
    // MANIFEST header/body is invalid.  A truncated final record is treated as
    // an incomplete crash write: the MANIFEST is truncated to the last complete
    // record (or replaced by a snapshot when the Env cannot truncate) and then
    // reopened for appending.
    Status recover();

    // Creates a fresh MANIFEST containing a full snapshot of the current state
    // and atomically points CURRENT at it.  Used to create a new database, to
    // rotate a MANIFEST that grew past the rotation threshold, and as the
    // fallback for recovering a torn tail on an Env without truncate support.
    Status write_snapshot();

    // Appends one edit as a MANIFEST record, syncs it, then installs a new
    // Version.  The edit is validated before anything is written, so an invalid
    // edit does not leave a bad record in the MANIFEST.  Rotates the MANIFEST
    // when it exceeds the configured size.
    Status log_and_apply(const VersionEdit &edit);

    // Returns the next unused file number and advances the allocator.  Numbers
    // are monotonic for the lifetime of the VersionSet and never reused.
    uint64_t new_file_number();

    // Ensures later allocations are greater than `number`.
    void mark_file_number_used(uint64_t number);

    uint64_t manifest_number() const { return manifest_number_; }
    uint64_t log_number() const { return log_number_; }
    uint64_t next_file_number() const { return next_file_number_; }
    uint64_t last_timestamp() const { return last_timestamp_; }

    const std::string &comparator_name() const { return comparator_name_; }

    // Current immutable Version.  The caller must ref() it to keep it alive
    // across later VersionSet mutations, and unref() it when done.
    Version *current() const { return current_; }

    const std::vector<std::vector<FileMetaData>> &levels() const { return current_->levels(); }
    const std::vector<FileMetaData> &level_files(uint32_t level) const { return current_->level_files(level); }

    const std::map<uint32_t, std::string> &compact_pointers() const { return compact_pointers_; }

    bool has_vlog_head() const { return has_vlog_head_; }
    bool has_vlog_tail() const { return has_vlog_tail_; }
    const VlogPosition &vlog_head() const { return vlog_head_; }
    const VlogPosition &vlog_tail() const { return vlog_tail_; }
    const std::map<uint32_t, VlogSegmentStat> &vlog_segment_stats() const { return vlog_segment_stats_; }

    // True when recover() found and dropped a truncated final record.
    bool tail_truncated() const { return tail_truncated_; }

    // Sticky status: non-ok once a MANIFEST write or apply has failed.
    const Status &status() const { return status_; }

    // Adds every live table/vLog file and the active WAL and MANIFEST to
    // *files.  Used during recovery to identify orphans in the directory.
    void get_live_files(std::set<std::string> *files) const;

    // Returns and clears the files made obsolete since the last call.  A file
    // is obsolete only when no live Version references it.  Removal is the
    // caller's job; a missing file can be ignored.
    std::vector<std::string> get_obsolete_files();

  private:
    friend class Version;

    Status read_current(uint64_t *manifest_number);
    Status replay_manifest(uint64_t manifest_number, std::vector<std::vector<FileMetaData>> *levels,
                           bool *tail_truncated, uint64_t *truncate_offset);

    Status build_snapshot(VersionEdit *snapshot) const;
    Status write_current(uint64_t manifest_number);

    Status validate_edit_against(const VersionEdit &edit, const std::vector<std::vector<FileMetaData>> &levels) const;
    // Mutates `levels` and the VersionSet metadata; does not install a Version.
    Status apply_edit_fields(const VersionEdit &edit, std::vector<std::vector<FileMetaData>> *levels,
                             bool track_obsolete);
    // Copies the current levels, applies the edit, and installs a new Version.
    Status apply_edit(const VersionEdit &edit);
    void sort_levels(std::vector<std::vector<FileMetaData>> *levels) const;

    // Version lifetime and file bookkeeping.
    void clear_versions();
    void link_version(Version *version);
    void unlink_version(Version *version);
    void add_pins(Version *version);
    void remove_pins(Version *version);
    Version *create_version(std::vector<std::vector<FileMetaData>> levels);
    void install_version(Version *version);
    void unref_version_(Version *version);
    std::vector<uint32_t> current_vlog_ids() const;

    void add_obsolete_table(uint64_t table_id);
    void add_obsolete_vlog(uint32_t file_id);

    Env *env_;
    Comparator comparator_;
    std::string dbname_;
    size_t num_levels_;
    uint64_t manifest_max_bytes_;

    uint64_t manifest_number_ = 0;
    uint64_t log_number_ = 0;
    uint64_t next_file_number_ = 1;
    uint64_t last_timestamp_ = 0;

    std::string comparator_name_;
    bool has_comparator_name_ = false;

    // Intrusive list head of all live Versions; current_ is always a member.
    Version *versions_ = nullptr;
    Version *current_ = nullptr;

    // How many live Versions reference each table / vLog segment.  Reaching
    // zero makes the corresponding files obsolete.
    std::map<uint64_t, int> table_pins_;
    std::map<uint32_t, int> vlog_pins_;
    std::set<std::string> obsolete_files_;

    std::map<uint32_t, std::string> compact_pointers_;
    bool has_vlog_head_ = false;
    bool has_vlog_tail_ = false;
    VlogPosition vlog_head_;
    VlogPosition vlog_tail_;
    std::map<uint32_t, VlogSegmentStat> vlog_segment_stats_;

    ManifestFile *manifest_ = nullptr;

    bool recovered_ = false;
    bool tail_truncated_ = false;
    Status status_;
};

} // namespace ANCHORDB_NAMESPACE
