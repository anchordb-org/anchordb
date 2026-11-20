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

#include "db/flush.h"

#include "memtable/entry.h"
#include "memtable/table.h"
#include "sstable/table_builder.h"
#include "sstable/table_file_set.h"
#include <cstdint>
#include <string>
#include <utility>

namespace ANCHORDB_NAMESPACE {

FlushJob::FlushJob(const Options &options, std::string dbname, uint64_t table_number, const MemTable &memtable)
    : options_(options), dbname_(std::move(dbname)), table_number_(table_number), memtable_(memtable) {}

Status FlushJob::run(FileMetaData *out) {
    if (out == nullptr) { return Status::invalid_argument("flush job: null metadata"); }
    *out = FileMetaData();

    const TableFileSet files = make_table_file_set(dbname_, table_number_);
    TableBuilder builder(options_, files);
    if (!builder.status().is_ok()) { return builder.status(); }

    // Any early return leaves the builder unfinished, so its destructor calls
    // abort() and removes the partial table files.
    auto iter = memtable_.new_raw_iterator();
    for (iter.seek_to_first(); iter.valid(); iter.next()) {
        const MemtableEntry entry = iter.entry();
        Status s = iter.status();
        if (!s.is_ok()) { return s; }

        // A blob entry's value is its encoded BlobHandle; add() detects the
        // type and writes only an index slot instead of a data record.
        s = builder.add(entry.key(), entry.value(), entry.type(), entry.timestamp());
        if (!s.is_ok()) { return s; }
    }

    Status iter_status = iter.status();
    if (!iter_status.is_ok()) { return iter_status; }

    if (builder.entry_count() == 0) {
        builder.abort();
        return Status::ok();
    }

    Status s = builder.finish();
    if (!s.is_ok()) { return s; }

    out->table_id = table_number_;
    out->smallest_key.assign(builder.smallest_key());
    out->largest_key.assign(builder.largest_key());
    out->min_ts = builder.min_ts();
    out->max_ts = builder.max_ts();
    out->data_size = builder.data_size();
    out->index_size = builder.index_size();
    out->summary_size = builder.summary_size();
    out->filter_size = builder.filter_size();
    out->entry_count = builder.entry_count();
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
