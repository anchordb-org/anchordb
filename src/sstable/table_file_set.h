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
#include "file/filename.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// The files that make up one SSTable.  Names are produced by the shared
// file-naming helpers in file/filename.h.  `filter` is empty for tables built
// without a filter.
struct TableFileSet {
    std::string data;
    std::string index;
    std::string summary;
    std::string filter;
};

// Builds the .data / .index / .summary / .filter file names for
// `table_number` under `db_dir`.
inline TableFileSet make_table_file_set(std::string_view db_dir, uint64_t table_number) {
    TableFileSet set;
    set.data = table_data_filename(db_dir, table_number);
    set.index = table_index_filename(db_dir, table_number);
    set.summary = table_summary_filename(db_dir, table_number);
    set.filter = table_filter_filename(db_dir, table_number);
    return set;
}

} // namespace ANCHORDB_NAMESPACE
