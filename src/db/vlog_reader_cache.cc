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

#include "db/vlog_reader_cache.h"

#include "anchordb/env.h"
#include "anchordb/status.h"
#include "sstable/data_record.h"
#include <cstring>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

VLogReaderCache::VLogReaderCache(Env *env, Cache *borrowed_cache_) : env_(env), cache_(borrowed_cache_) {}

Status VLogReaderCache::acquire(std::string_view filename, Cache::Handle *pin) {
    if (pin == nullptr) { return Status::invalid_argument("vlog reader cache: null pin"); }
    if (env_ == nullptr) { return Status::not_supported("vlog reader cache: no env"); }
    if (!cache_.valid()) { return Status::invalid_argument("vlog reader cache: no cache"); }

    auto open_file = [this, filename](VLogFile **out) -> Status {
        if (out == nullptr) { return Status::invalid_argument("vlog reader cache: null output"); }
        *out = nullptr;

        const std::string name(filename);
        uint64_t file_size = 0;
        Status s = env_->get_file_size(name, &file_size);
        if (!s.is_ok()) { return s; }
        if (file_size < kVLogFileHeaderSize) {
            return Status::corruption("vlog reader cache: file shorter than header", std::to_string(file_size));
        }

        RandomAccessFileReader *reader = nullptr;
        s = env_->new_random_access_file_reader(name, &reader);
        if (!s.is_ok()) { return s; }

        std::string header;
        header.resize(kVLogFileHeaderSize);
        std::string_view view;
        s = reader->read(0, kVLogFileHeaderSize, &view, header.data());
        if (!s.is_ok()) {
            delete reader;
            return s;
        }
        if (view.size() < kVLogFileHeaderSize) {
            delete reader;
            return Status::corruption("vlog reader cache: short header read", std::to_string(view.size()));
        }
        if (view.data() != header.data()) { std::memmove(header.data(), view.data(), kVLogFileHeaderSize); }

        uint32_t codec_id = 0;
        uint32_t version = 0;
        s = parse_vlog_file_header(std::string_view(header.data(), kVLogFileHeaderSize), &codec_id, &version);
        if (!s.is_ok()) {
            delete reader;
            return s;
        }

        const Compressor compressor = new_compressor_by_id(codec_id);
        if (codec_id != 0 && (compressor.uncompress == nullptr || compressor.get_uncompressed_length == nullptr)) {
            delete reader;
            return Status::corruption("vlog reader cache: unsupported compression codec", std::to_string(codec_id));
        }

        auto *file = new VLogFile();
        file->reader = reader;
        file->format_version = version;
        file->codec_id = codec_id;
        file->compressor = compressor;
        *out = file;
        return Status::ok();
    };
    auto charge = [](VLogFile *) { return kVLogReaderCacheCharge + sizeof(VLogFile); };

    return cache_lookup_or_open<VLogFile>(cache_, filename, pin, /*value=*/nullptr, open_file, charge, &delete_entry);
}

void VLogReaderCache::erase(std::string_view filename) {
    if (cache_.valid()) { cache_.erase(filename); }
}

void VLogReaderCache::delete_entry(std::string_view, void *value) {
    auto *file = static_cast<VLogFile *>(value);
    delete file->reader;
    delete file;
}

} // namespace ANCHORDB_NAMESPACE
