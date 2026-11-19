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
#include <cstddef>
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
    uint64_t number = 0;
    // Sum of the table's four files (data, index, summary, filter).
    uint64_t file_size = 0;
    size_t entry_count = 0;
    std::string smallest_key;
    std::string largest_key;
    uint64_t smallest_timestamp = 0;
    uint64_t largest_timestamp = 0;
};

} // namespace ANCHORDB_NAMESPACE
