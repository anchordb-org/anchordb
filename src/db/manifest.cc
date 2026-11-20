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

#include "db/manifest.h"

#include "anchordb/env.h"
#include "anchordb/status.h"

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

Status ManifestFile::create(Env *env, const std::string &path, ManifestFile **result) {
    if (env == nullptr || result == nullptr) { return Status::invalid_argument("manifest: null env or result"); }
    *result = nullptr;

    WritableFileWriter *file = nullptr;
    Status s = env->new_writable_file_writer(path, &file);
    if (!s.is_ok()) { return s; }

    ManifestFile *manifest = new ManifestFile();
    manifest->file_ = file;
    // Writes the file header; a failure is sticky and surfaces on the first
    // append()/sync().
    manifest->log_ = new ManifestWriter(file);
    *result = manifest;
    return Status::ok();
}

Status ManifestFile::open_append(Env *env, const std::string &path, ManifestFile **result) {
    if (env == nullptr || result == nullptr) { return Status::invalid_argument("manifest: null env or result"); }
    *result = nullptr;

    WritableFileWriter *file = nullptr;
    Status s = env->new_appendable_file_writer(path, &file);
    if (!s.is_ok()) { return s; }

    uint64_t file_size = 0;
    s = env->get_file_size(path, &file_size);
    if (!s.is_ok()) {
        (void)file->close();
        delete file;
        return s;
    }

    ManifestFile *manifest = new ManifestFile();
    manifest->file_ = file;
    manifest->log_ = new ManifestWriter(file, kLogAppend, file_size);
    *result = manifest;
    return Status::ok();
}

ManifestFile::~ManifestFile() { (void)close(); }

Status ManifestFile::append(const VersionEdit &edit) {
    if (log_ == nullptr) { return Status::invalid_argument("manifest: file is not open"); }
    return edit.append_to(log_);
}

Status ManifestFile::flush() {
    if (log_ == nullptr) { return Status::invalid_argument("manifest: file is not open"); }
    return log_->flush();
}

Status ManifestFile::sync() {
    if (log_ == nullptr) { return Status::invalid_argument("manifest: file is not open"); }
    return log_->sync();
}

uint64_t ManifestFile::bytes_written() const { return log_ == nullptr ? 0 : log_->bytes_written(); }

Status ManifestFile::close() {
    delete log_;
    log_ = nullptr;

    if (file_ == nullptr) { return Status::ok(); }
    Status s = file_->close();
    delete file_;
    file_ = nullptr;
    return s;
}

Status ManifestIterator::open(Env *env, const std::string &path, ManifestIterator **result) {
    if (env == nullptr || result == nullptr) { return Status::invalid_argument("manifest: null env or result"); }
    *result = nullptr;

    uint64_t file_size = 0;
    Status s = env->get_file_size(path, &file_size);
    if (!s.is_ok()) { return s; }
    if (file_size < kManifestFileHeaderSize) { return Status::corruption("manifest: empty or truncated", path); }

    SequentialFileReader *reader = nullptr;
    s = env->new_sequential_file_reader(path, &reader);
    if (!s.is_ok()) { return s; }

    ManifestIterator *iterator = new ManifestIterator();
    iterator->reader_ = reader;
    iterator->size_ = file_size;
    iterator->log_ = new ManifestReader(reader, static_cast<size_t>(file_size));
    *result = iterator;
    return Status::ok();
}

ManifestIterator::~ManifestIterator() {
    delete log_;
    log_ = nullptr;
    delete reader_;
    reader_ = nullptr;
}

Status ManifestIterator::next(VersionEdit *edit) {
    if (log_ == nullptr) { return Status::invalid_argument("manifest: iterator is not open"); }
    if (edit == nullptr) { return Status::invalid_argument("manifest: null edit"); }
    return edit->read_from(log_);
}

} // namespace ANCHORDB_NAMESPACE
