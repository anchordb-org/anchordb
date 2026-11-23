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
#include "anchordb/options.h"
#include "anchordb/status.h"
#include "db/vlog_reader_cache.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

struct BlobHandle;
class Cache;
class Env;

// The DB layer's single value-resolution path for the WiscKey value log.
//
// A BlobHandle stored in the memtable or an SSTable index addresses a complete
// record in a vLog file.  Both storage layers hand that handle here instead of
// doing vLog I/O themselves, so the read, range check, record decode and
// key/type validation live in exactly one place.
//
// The resolver owns the reader cache, so open vLog descriptors are pooled and
// shared across every memtable and table.  It also owns the database directory
// used to build the numbered vLog file names.
//
// Ownership: not reference counted.  The DB layer owns one instance and lends
// it only to its read path (point reads and ResolvingIterator); the storage
// layers no longer borrow it.  The owner must outlive every reader.
class BlobResolver {
  public:
    // Takes ownership of `cache`; it must be non-null until destruction.
    // `db_dir` prefixes the numbered vLog file names.  `max_vlog_value_size`
    // rejects a resolved blob larger than this before allocating it (0
    // disables the limit); wire it from Options::max_vlog_value_size.
    BlobResolver(Env *env, std::string db_dir, Cache *cache, size_t max_vlog_value_size = kDefaultMaxVLogValueSize);

    BlobResolver(const BlobResolver &) = delete;
    BlobResolver &operator=(const BlobResolver &) = delete;

    // Resolves `handle` to its blob payload, verifies that the referenced
    // record is a blob whose key matches `expected_key`, and copies the value
    // into `*value`.  `scratch` is reusable per-call storage for the raw record
    // bytes; pass nullptr to use internal storage.  Returns Status::ok() on
    // success and a corruption/IO status otherwise.
    Status resolve(const BlobHandle &handle, std::string_view expected_key, std::string *value,
                   std::string *scratch) const;

    // Drops the cached reader for the vLog file `file_id` (for example after
    // the file is removed).  The reader stays alive until any outstanding read
    // finishes.
    void erase_file(uint32_t file_id);

    size_t total_charge() const { return readers_.total_charge(); }

  private:
    Env *env_;
    std::string db_dir_;
    // Ceiling on a resolved blob's size (decompressed for Compressed records);
    // zero means unlimited.
    size_t max_vlog_value_size_ = kDefaultMaxVLogValueSize;
    // Mutable so resolve() can populate the reader pool from a const resolver;
    // VLogReaderCache is internally synchronized.
    mutable VLogReaderCache readers_;
};

} // namespace ANCHORDB_NAMESPACE
