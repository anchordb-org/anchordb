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

#include "db/blob_resolver.h"

#include "anchordb/cache.h"
#include "anchordb/env.h"
#include "db/db_format.h"
#include "file/filename.h"
#include "sstable/data_record.h"
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

BlobResolver::BlobResolver(Env *env, std::string db_dir, Cache *cache)
    : env_(env), db_dir_(std::move(db_dir)), readers_(env, cache) {}

Status BlobResolver::resolve(const BlobHandle &handle, std::string_view expected_key, std::string *value,
                             std::string *scratch) const {
    if (value == nullptr) { return Status::invalid_argument("blob resolver: null value"); }
    if (env_ == nullptr) { return Status::not_supported("blob resolver: no env"); }
    if (!readers_.valid()) { return Status::not_supported("blob resolver: no reader cache"); }
    if (handle.size < kDataRecordHeaderSize) {
        return Status::corruption("blob resolver: vlog record size too small", std::to_string(handle.size));
    }

    const std::string filename = vlog_filename(db_dir_, handle.file_id);
    // Use the live file size: a reader cached for this file may predate later
    // appends to the same vLog file.
    uint64_t file_size = 0;
    Status s = env_->get_file_size(filename, &file_size);
    if (!s.is_ok()) { return s; }
    if (handle.offset > file_size || handle.size > file_size - handle.offset) {
        return Status::corruption("blob resolver: vlog record out of range", std::to_string(handle.offset));
    }

    Cache::PinnedHandle pin;
    s = readers_.open(filename, &pin);
    if (!s.is_ok()) { return s; }

    auto *reader = static_cast<RandomAccessFileReader *>(pin.value());
    std::string local_scratch;
    std::string *buf = scratch != nullptr ? scratch : &local_scratch;
    buf->resize(handle.size);

    std::string_view view;
    s = reader->read(handle.offset, handle.size, &view, buf->data());
    if (!s.is_ok()) { return s; }
    if (view.size() < handle.size) {
        return Status::corruption("blob resolver: vlog record short read", std::to_string(view.size()));
    }
    if (view.data() != buf->data()) { std::memmove(buf->data(), view.data(), handle.size); }

    DataRecordBufferReader record_reader(std::string_view(buf->data(), handle.size));
    ParsedDataRecord record;
    bool done = false;
    s = record_reader.next(&record, &done);
    if (!s.is_ok()) { return s; }
    if (done) { return Status::corruption("blob resolver: vlog record missing"); }
    if (!record_reader.done()) { return Status::corruption("blob resolver: vlog record has trailing bytes"); }
    if (!record.is_blob()) { return Status::corruption("blob resolver: vlog record is not a blob"); }
    if (record.key != expected_key) { return Status::corruption("blob resolver: vlog record key mismatch"); }

    value->assign(record.value.data(), record.value.size());
    return Status::ok();
}

void BlobResolver::erase_file(uint32_t file_id) { readers_.erase(vlog_filename(db_dir_, file_id)); }

} // namespace ANCHORDB_NAMESPACE
