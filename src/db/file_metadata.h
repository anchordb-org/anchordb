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
#include <cstdint>
#include <string>

namespace ANCHORDB_NAMESPACE {

// Durable description of one SSTable, recorded in the MANIFEST by whoever
// created the table (flush or compaction) and read back during recovery.
//
// Bounds are user keys and are taken from the TableBuilder that wrote the
// table; they are never derived by opening the table.  L0 tables may overlap;
// L1+ tables are non-overlapping and kept sorted by smallest_key.  The
// timestamps let a read skip a table whose whole range is older than the
// requested snapshot.
struct FileMetaData {
    uint64_t table_id = 0;
    std::string smallest_key;
    std::string largest_key;
    uint64_t min_ts = 0;
    uint64_t max_ts = 0;

    // Individual component file sizes in bytes
    uint64_t data_size = 0;
    uint64_t index_size = 0;
    uint64_t summary_size = 0;
    uint64_t filter_size = 0; // 0 indicates no filter file was written

    uint64_t entry_count = 0; // Fixed-width uint64_t for deterministic serialization

    // Helper for level size accounting & compaction scoring
    uint64_t total_file_size() const { return data_size + index_size + summary_size + filter_size; }

    // Helper to check if a filter file exists on disk
    bool has_filter() const { return filter_size > 0; }
};

} // namespace ANCHORDB_NAMESPACE
