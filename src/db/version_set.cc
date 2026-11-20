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

#include "db/version_set.h"

#include "db/manifest.h"
#include "file/filename.h"
#include <algorithm>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// A CURRENT file is one short line; bound the read so a corrupted file cannot
// make recovery allocate without limit.
constexpr size_t kMaxCurrentFileSize = 4096;

int byte_wise_compare(std::string_view a, std::string_view b) { return a.compare(b); }

} // namespace

VersionSet::VersionSet(const Options &options, std::string dbname, size_t num_levels)
    : env_(options.env), comparator_(options.comparator), dbname_(std::move(dbname)), num_levels_(num_levels),
      manifest_max_bytes_(options.manifest_max_file_size) {
    current_ = create_version(std::vector<std::vector<FileMetaData>>(num_levels_));
    link_version(current_);
    add_pins(current_);
}

VersionSet::~VersionSet() {
    clear_versions();
    delete manifest_;
    manifest_ = nullptr;
}

Status VersionSet::recover() {
    if (recovered_) { return Status::invalid_argument("version set: already recovered"); }

    // Start from a clean in-memory image while keeping the configured Env and
    // comparator.
    clear_versions();
    delete manifest_;
    manifest_ = nullptr;

    manifest_number_ = 0;
    log_number_ = 0;
    next_file_number_ = 1;
    last_timestamp_ = 0;
    comparator_name_.clear();
    has_comparator_name_ = false;
    compact_pointers_.clear();
    has_vlog_head_ = false;
    has_vlog_tail_ = false;
    vlog_head_ = VlogPosition{};
    vlog_tail_ = VlogPosition{};
    vlog_segment_stats_.clear();
    obsolete_files_.clear();
    recovered_ = false;
    tail_truncated_ = false;
    status_ = Status::ok();

    current_ = create_version(std::vector<std::vector<FileMetaData>>(num_levels_));
    link_version(current_);
    add_pins(current_);

    uint64_t manifest_number = 0;
    Status s = read_current(&manifest_number);
    if (!s.is_ok()) { return s; }

    manifest_number_ = manifest_number;
    mark_file_number_used(manifest_number_);

    std::vector<std::vector<FileMetaData>> levels(num_levels_);
    bool truncated = false;
    uint64_t truncate_offset = 0;
    s = replay_manifest(manifest_number, &levels, &truncated, &truncate_offset);
    if (!s.is_ok()) { return s; }

    if (has_comparator_name_ && comparator_.name() != nullptr && comparator_name_ != comparator_.name()) {
        return Status::corruption("version set: comparator mismatch", comparator_name_);
    }

    // Guard the allocator against every id the recovered image references,
    // even if a next_file_number record was lost.
    mark_file_number_used(log_number_);
    for (const std::vector<FileMetaData> &files : levels) {
        for (const FileMetaData &meta : files) { mark_file_number_used(meta.table_id); }
    }
    if (has_vlog_head_) { mark_file_number_used(vlog_head_.file_id); }
    if (has_vlog_tail_) { mark_file_number_used(vlog_tail_.file_id); }
    for (const auto &entry : vlog_segment_stats_) { mark_file_number_used(entry.first); }

    // Publish the recovered state as a single immutable Version.
    install_version(create_version(std::move(levels)));

    tail_truncated_ = truncated;
    if (truncated) {
        // Drop the torn tail in place, then append after the last complete
        // record.  If the Env cannot truncate, fall back to a fresh snapshot.
        s = env_->truncate_file(descriptor_filename(dbname_, manifest_number), truncate_offset);
        if (s.is_not_supported()) { return write_snapshot(); }
        if (!s.is_ok()) { return s; }
    }

    ManifestFile *manifest = nullptr;
    s = ManifestFile::open_append(env_, descriptor_filename(dbname_, manifest_number), &manifest);
    if (!s.is_ok()) { return s; }
    manifest_ = manifest;

    recovered_ = true;
    return Status::ok();
}

Status VersionSet::write_snapshot() {
    if (!status_.is_ok()) { return status_; }

    const uint64_t number = new_file_number();
    const std::string path = descriptor_filename(dbname_, number);

    ManifestFile *manifest = nullptr;
    Status s = ManifestFile::create(env_, path, &manifest);
    if (!s.is_ok()) { return s; }

    VersionEdit snapshot;
    s = build_snapshot(&snapshot);
    if (s.is_ok()) { s = manifest->append(snapshot); }
    if (s.is_ok()) { s = manifest->sync(); }
    if (!s.is_ok()) {
        delete manifest;
        (void)env_->remove_file(path);
        return s;
    }

    s = write_current(number);
    if (!s.is_ok()) {
        delete manifest;
        (void)env_->remove_file(path);
        return s;
    }

    // The new MANIFEST is durable and CURRENT points at it.  The old descriptor
    // is now obsolete; closing it cannot invalidate the new state.
    if (manifest_number_ != 0) { obsolete_files_.insert(descriptor_filename(dbname_, manifest_number_)); }
    delete manifest_;
    manifest_ = manifest;
    manifest_number_ = number;
    recovered_ = true;
    return Status::ok();
}

Status VersionSet::log_and_apply(const VersionEdit &edit) {
    if (!status_.is_ok()) { return status_; }
    if (manifest_ == nullptr) { return Status::invalid_argument("version set: MANIFEST is not open"); }

    // Validate before writing so a bad edit never enters the MANIFEST.
    Status s = validate_edit_against(edit, current_->levels());
    if (!s.is_ok()) { return s; }

    s = manifest_->append(edit);
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }

    // The edit must be durable before it can affect reads.
    s = manifest_->sync();
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }

    s = apply_edit(edit);
    if (!s.is_ok()) {
        status_ = s;
        return status_;
    }

    if (manifest_max_bytes_ != 0 && manifest_->bytes_written() >= manifest_max_bytes_) {
        s = write_snapshot();
        if (!s.is_ok()) { status_ = s; }
    }
    return s;
}

uint64_t VersionSet::new_file_number() { return next_file_number_++; }

void VersionSet::mark_file_number_used(uint64_t number) {
    if (number >= next_file_number_) { next_file_number_ = number + 1; }
}

void VersionSet::get_live_files(std::set<std::string> *files) const {
    if (files == nullptr) { return; }

    for (Version *version = versions_; version != nullptr; version = version->next_) {
        for (const std::vector<FileMetaData> &level : version->levels()) {
            for (const FileMetaData &meta : level) {
                files->insert(table_data_filename(dbname_, meta.table_id));
                files->insert(table_index_filename(dbname_, meta.table_id));
                files->insert(table_summary_filename(dbname_, meta.table_id));
                files->insert(table_filter_filename(dbname_, meta.table_id));
            }
        }
        for (uint32_t file_id : version->vlog_segments()) { files->insert(vlog_filename(dbname_, file_id)); }
    }

    if (log_number_ != 0) { files->insert(log_filename(dbname_, log_number_)); }
    if (manifest_number_ != 0) { files->insert(descriptor_filename(dbname_, manifest_number_)); }
    files->insert(current_filename(dbname_));
}

std::vector<std::string> VersionSet::get_obsolete_files() {
    std::vector<std::string> result(obsolete_files_.begin(), obsolete_files_.end());
    obsolete_files_.clear();
    return result;
}

Status VersionSet::read_current(uint64_t *manifest_number) {
    if (manifest_number == nullptr) { return Status::invalid_argument("version set: null CURRENT result"); }

    const std::string path = current_filename(dbname_);
    SequentialFileReader *reader = nullptr;
    Status s = env_->new_sequential_file_reader(path, &reader);
    if (!s.is_ok()) { return s; }

    std::string contents;
    char scratch[256];
    for (;;) {
        std::string_view view;
        s = reader->read(sizeof(scratch), &view, scratch);
        if (!s.is_ok()) {
            delete reader;
            return s;
        }
        if (view.empty()) { break; }
        contents.append(view.data(), view.size());
        if (contents.size() > kMaxCurrentFileSize) {
            delete reader;
            return Status::corruption("version set: CURRENT is too long", path);
        }
    }
    delete reader;

    while (!contents.empty() &&
           (contents.back() == '\n' || contents.back() == '\r' || contents.back() == ' ' || contents.back() == '\t')) {
        contents.pop_back();
    }
    if (contents.empty()) { return Status::corruption("version set: CURRENT is empty", path); }

    uint64_t number = 0;
    FileType type = FileType::kCurrentFile;
    if (!parse_filename(contents, &number, &type) || type != FileType::kDescriptorFile) {
        return Status::corruption("version set: CURRENT does not name a MANIFEST", contents);
    }

    *manifest_number = number;
    return Status::ok();
}

Status VersionSet::replay_manifest(uint64_t manifest_number, std::vector<std::vector<FileMetaData>> *levels,
                                   bool *tail_truncated, uint64_t *truncate_offset) {
    if (levels == nullptr) { return Status::invalid_argument("version set: null replay levels"); }
    if (tail_truncated == nullptr) { return Status::invalid_argument("version set: null tail result"); }
    if (truncate_offset == nullptr) { return Status::invalid_argument("version set: null truncate result"); }
    *tail_truncated = false;
    *truncate_offset = 0;

    const std::string path = descriptor_filename(dbname_, manifest_number);
    ManifestIterator *iterator = nullptr;
    Status s = ManifestIterator::open(env_, path, &iterator);
    if (!s.is_ok()) { return s; }

    Status result = Status::ok();
    while (!iterator->done()) {
        VersionEdit edit;
        s = iterator->next(&edit);
        if (!s.is_ok()) {
            // A failure with no bytes consumed means the header itself is bad.
            if (iterator->position() == 0) {
                result = s;
                break;
            }
            // A torn tail is the crash case: drop it.  position() is the start
            // of the record that failed, i.e. the last complete record
            // boundary, so the caller can truncate there.
            *tail_truncated = true;
            *truncate_offset = iterator->position();
            break;
        }

        s = validate_edit_against(edit, *levels);
        if (!s.is_ok()) {
            result = s;
            break;
        }
        s = apply_edit_fields(edit, levels, false);
        if (!s.is_ok()) {
            result = s;
            break;
        }
    }
    delete iterator;

    return result;
}

Status VersionSet::build_snapshot(VersionEdit *snapshot) const {
    if (snapshot == nullptr) { return Status::invalid_argument("version set: null snapshot"); }
    snapshot->clear();

    if (has_comparator_name_) {
        snapshot->set_comparator_name(comparator_name_);
    } else if (comparator_.name() != nullptr) {
        snapshot->set_comparator_name(comparator_.name());
    }

    snapshot->set_log_number(log_number_);
    snapshot->set_next_file_number(next_file_number_);
    snapshot->set_last_timestamp(last_timestamp_);

    for (const auto &entry : compact_pointers_) { snapshot->add_compact_pointer(entry.first, entry.second); }

    for (size_t level = 0; level < current_->levels().size(); ++level) {
        for (const FileMetaData &meta : current_->levels()[level]) {
            snapshot->add_table(static_cast<uint32_t>(level), meta);
        }
    }

    if (has_vlog_head_) { snapshot->set_vlog_head(vlog_head_.file_id, vlog_head_.offset); }
    if (has_vlog_tail_) { snapshot->set_vlog_tail(vlog_tail_.file_id, vlog_tail_.offset); }
    for (const auto &entry : vlog_segment_stats_) {
        snapshot->add_vlog_segment_stat(entry.first, entry.second.total_size, entry.second.stale_bytes);
    }

    return Status::ok();
}

Status VersionSet::write_current(uint64_t manifest_number) {
    const std::string current = current_filename(dbname_);
    const std::string temp = current + ".tmp";

    WritableFileWriter *writer = nullptr;
    Status s = env_->new_writable_file_writer(temp, &writer);
    if (!s.is_ok()) { return s; }

    std::string contents = descriptor_filename(std::string_view(), manifest_number);
    contents.push_back('\n');

    s = writer->append(contents);
    if (s.is_ok()) { s = writer->sync(); }
    Status close_status = writer->close();
    delete writer;
    if (s.is_ok()) { s = close_status; }
    if (!s.is_ok()) {
        env_->remove_file(temp);
        return s;
    }

    s = env_->rename_file(temp, current);
    if (!s.is_ok()) { return s; }

    const std::string directory = dbname_.empty() ? std::string(".") : dbname_;
    return env_->sync_dir(directory);
}

Status VersionSet::validate_edit_against(const VersionEdit &edit,
                                         const std::vector<std::vector<FileMetaData>> &levels) const {
    if (edit.has_comparator_name()) {
        if (comparator_.name() != nullptr && edit.comparator_name() != comparator_.name()) {
            return Status::corruption("version set: comparator mismatch", edit.comparator_name());
        }
        if (has_comparator_name_ && edit.comparator_name() != comparator_name_) {
            return Status::corruption("version set: comparator changes are not allowed", edit.comparator_name());
        }
    }

    if (edit.has_log_number() && log_number_ != 0 && edit.log_number() < log_number_) {
        return Status::corruption("version set: log number moved backwards", std::to_string(edit.log_number()));
    }

    for (const CompactPointer &pointer : edit.compact_pointers()) {
        if (pointer.level >= num_levels_) {
            return Status::corruption("version set: compact pointer level out of range", std::to_string(pointer.level));
        }
    }

    for (const DeletedTable &table : edit.deleted_tables()) {
        if (table.level >= num_levels_) {
            return Status::corruption("version set: deleted table level out of range", std::to_string(table.level));
        }
    }

    int (*compare)(std::string_view, std::string_view) = comparator_.compare;
    if (compare == nullptr) { compare = &byte_wise_compare; }

    const std::vector<AddedTable> &added = edit.added_tables();
    for (size_t i = 0; i < added.size(); ++i) {
        const AddedTable &table = added[i];
        if (table.level >= num_levels_) {
            return Status::corruption("version set: added table level out of range", std::to_string(table.level));
        }
        if (compare(table.meta.smallest_key, table.meta.largest_key) > 0) {
            return Status::corruption("version set: table smallest_key exceeds largest_key",
                                      std::to_string(table.meta.table_id));
        }

        const uint64_t table_id = table.meta.table_id;
        for (const std::vector<FileMetaData> &files : levels) {
            for (const FileMetaData &meta : files) {
                if (meta.table_id == table_id) {
                    return Status::corruption("version set: duplicate table id", std::to_string(table_id));
                }
            }
        }
        for (size_t j = 0; j < i; ++j) {
            if (added[j].meta.table_id == table_id) {
                return Status::corruption("version set: duplicate table id in edit", std::to_string(table_id));
            }
        }
    }

    return Status::ok();
}

Status VersionSet::apply_edit_fields(const VersionEdit &edit, std::vector<std::vector<FileMetaData>> *levels,
                                     bool track_obsolete) {
    if (levels == nullptr) { return Status::invalid_argument("version set: null apply levels"); }

    if (edit.has_comparator_name()) {
        comparator_name_ = edit.comparator_name();
        has_comparator_name_ = true;
    }
    if (edit.has_log_number()) {
        if (track_obsolete && log_number_ != 0 && edit.log_number() > log_number_) {
            obsolete_files_.insert(log_filename(dbname_, log_number_));
        }
        log_number_ = edit.log_number();
        mark_file_number_used(log_number_);
    }
    if (edit.has_next_file_number() && edit.next_file_number() > next_file_number_) {
        next_file_number_ = edit.next_file_number();
    }
    if (edit.has_last_timestamp() && edit.last_timestamp() > last_timestamp_) {
        last_timestamp_ = edit.last_timestamp();
    }

    for (const CompactPointer &pointer : edit.compact_pointers()) { compact_pointers_[pointer.level] = pointer.key; }

    for (const DeletedTable &table : edit.deleted_tables()) {
        std::vector<FileMetaData> &files = (*levels)[table.level];
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [&table](const FileMetaData &meta) { return meta.table_id == table.table_id; }),
                    files.end());
        mark_file_number_used(table.table_id);
    }

    for (const AddedTable &table : edit.added_tables()) {
        (*levels)[table.level].push_back(table.meta);
        mark_file_number_used(table.meta.table_id);
    }

    if (edit.has_vlog_head()) {
        vlog_head_ = edit.vlog_head();
        has_vlog_head_ = true;
        mark_file_number_used(vlog_head_.file_id);
    }
    if (edit.has_vlog_tail()) {
        vlog_tail_ = edit.vlog_tail();
        has_vlog_tail_ = true;
        mark_file_number_used(vlog_tail_.file_id);
    }

    for (const VlogSegmentStat &stat : edit.vlog_segment_stats()) {
        vlog_segment_stats_[stat.file_id] = stat;
        mark_file_number_used(stat.file_id);
    }
    for (uint32_t file_id : edit.deleted_vlog_segments()) {
        vlog_segment_stats_.erase(file_id);
        mark_file_number_used(file_id);
    }

    sort_levels(levels);
    return Status::ok();
}

Status VersionSet::apply_edit(const VersionEdit &edit) {
    // Copy the current layout, apply, and publish a new immutable Version.  The
    // old Version is only unref'd, so readers pinning it keep their files.
    std::vector<std::vector<FileMetaData>> levels = current_->levels();
    Status s = apply_edit_fields(edit, &levels, true);
    if (!s.is_ok()) { return s; }

    install_version(create_version(std::move(levels)));
    return Status::ok();
}

void VersionSet::sort_levels(std::vector<std::vector<FileMetaData>> *levels) const {
    if (levels->empty()) { return; }

    std::sort((*levels)[0].begin(), (*levels)[0].end(),
              [](const FileMetaData &a, const FileMetaData &b) { return a.table_id > b.table_id; });

    int (*compare)(std::string_view, std::string_view) = comparator_.compare;
    if (compare == nullptr) { compare = &byte_wise_compare; }

    for (size_t level = 1; level < levels->size(); ++level) {
        std::sort((*levels)[level].begin(), (*levels)[level].end(),
                  [compare](const FileMetaData &a, const FileMetaData &b) {
                      const int c = compare(a.smallest_key, b.smallest_key);
                      if (c != 0) { return c < 0; }
                      return a.table_id < b.table_id;
                  });
    }
}

void VersionSet::clear_versions() {
    Version *version = versions_;
    while (version != nullptr) {
        Version *next = version->next_;
        version->next_ = nullptr;
        version->prev_ = nullptr;
        delete version;
        version = next;
    }
    versions_ = nullptr;
    current_ = nullptr;
    table_pins_.clear();
    vlog_pins_.clear();
}

void VersionSet::link_version(Version *version) {
    version->prev_ = nullptr;
    version->next_ = versions_;
    if (versions_ != nullptr) { versions_->prev_ = version; }
    versions_ = version;
}

void VersionSet::unlink_version(Version *version) {
    if (version->prev_ != nullptr) {
        version->prev_->next_ = version->next_;
    } else {
        versions_ = version->next_;
    }
    if (version->next_ != nullptr) { version->next_->prev_ = version->prev_; }
    version->next_ = nullptr;
    version->prev_ = nullptr;
}

void VersionSet::add_pins(Version *version) {
    for (const std::vector<FileMetaData> &files : version->levels()) {
        for (const FileMetaData &meta : files) { ++table_pins_[meta.table_id]; }
    }
    for (uint32_t file_id : version->vlog_segments()) { ++vlog_pins_[file_id]; }
}

void VersionSet::remove_pins(Version *version) {
    for (const std::vector<FileMetaData> &files : version->levels()) {
        for (const FileMetaData &meta : files) {
            auto it = table_pins_.find(meta.table_id);
            if (it == table_pins_.end()) { continue; }
            if (--it->second <= 0) {
                table_pins_.erase(it);
                add_obsolete_table(meta.table_id);
            }
        }
    }
    for (uint32_t file_id : version->vlog_segments()) {
        auto it = vlog_pins_.find(file_id);
        if (it == vlog_pins_.end()) { continue; }
        if (--it->second <= 0) {
            vlog_pins_.erase(it);
            add_obsolete_vlog(file_id);
        }
    }
}

Version *VersionSet::create_version(std::vector<std::vector<FileMetaData>> levels) {
    return new Version(this, std::move(levels), current_vlog_ids());
}

void VersionSet::install_version(Version *version) {
    Version *old = current_;
    current_ = version;
    link_version(version);
    add_pins(version);
    if (old != nullptr) { old->unref(); }
}

void VersionSet::unref_version_(Version *version) {
    unlink_version(version);
    remove_pins(version);
    delete version;
}

std::vector<uint32_t> VersionSet::current_vlog_ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(vlog_segment_stats_.size());
    for (const auto &entry : vlog_segment_stats_) { ids.push_back(entry.first); }
    return ids;
}

void VersionSet::add_obsolete_table(uint64_t table_id) {
    obsolete_files_.insert(table_data_filename(dbname_, table_id));
    obsolete_files_.insert(table_index_filename(dbname_, table_id));
    obsolete_files_.insert(table_summary_filename(dbname_, table_id));
    obsolete_files_.insert(table_filter_filename(dbname_, table_id));
}

void VersionSet::add_obsolete_vlog(uint32_t file_id) { obsolete_files_.insert(vlog_filename(dbname_, file_id)); }

} // namespace ANCHORDB_NAMESPACE
