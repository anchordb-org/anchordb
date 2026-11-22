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
#include "anchordb/compressor.h"
#include "anchordb/filter.h"
#include "anchordb/status.h"
#include "db/file_metadata.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace ANCHORDB_NAMESPACE {

class Env;
class MemTable;

// Writes one immutable memtable into a single SSTable through TableBuilder.
//
// The job iterates the memtable with new_raw_iterator(), so blob entries are
// copied as their encoded BlobHandle and no value is read back from the vLog.
// The memtable neither writes nor syncs the vLog any more: the DB write path
// must make a blob record durable before logging its handle, so a published
// table never references vLog bytes that are not yet durable.
// Entries are handed to TableBuilder in the memtable's native order (user key
// ascending, timestamp descending), which is the order TableBuilder enforces.
//
// The files are written under their final names, derived from `dbname` and
// `table_number`.  A table only becomes visible once the caller records the
// returned FileMetaData in the MANIFEST, so that edit is the publication point;
// if the job fails, TableBuilder::abort() removes the partial files.
//
// The caller must ensure `table_number` is unused and that the memtable has no
// concurrent writer.  `comparator` must match the comparator the memtable was
// built with.  The DB/orchestrator layer derives the format settings from
// Options.
class FlushJob {
  public:
    // `memtable` is borrowed and must outlive the job and any call to run().
    FlushJob(Env *env, std::string dbname, uint64_t table_number, const MemTable &memtable,
             const Comparator &comparator, const Compressor &compressor, int compression_level, size_t block_size,
             size_t max_key_size, const Filter &filter);

    FlushJob(const FlushJob &) = delete;
    FlushJob &operator=(const FlushJob &) = delete;

    // Builds, syncs and closes the table.  On success fills *out with the
    // table's metadata.  When the memtable is empty, removes the empty files,
    // leaves *out zero-initialized and returns ok.  `out` must not be null.
    Status run(FileMetaData *out);

  private:
    Env *env_;
    std::string dbname_;
    uint64_t table_number_;
    const MemTable &memtable_;
    Comparator comparator_;
    Compressor compressor_;
    int compression_level_;
    size_t block_size_;
    size_t max_key_size_;
    Filter filter_;
};

} // namespace ANCHORDB_NAMESPACE
