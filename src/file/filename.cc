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

#include "file/filename.h"
#include <cstdio>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Appends "/name" to `dbname`, or just `name` when `dbname` is empty.
std::string join_path(std::string_view dbname, std::string_view name) {
    std::string result;
    result.reserve(dbname.size() + name.size() + 1);
    if (!dbname.empty()) {
        result.append(dbname.data(), dbname.size());
        result.push_back('/');
    }
    result.append(name.data(), name.size());
    return result;
}

// Consumes a run of decimal digits from the front of *input, storing the value
// in *number.  Returns false if there is no leading digit or the value
// overflows.
bool consume_decimal_number(std::string_view *input, uint64_t *number) {
    uint64_t value = 0;
    size_t consumed = 0;
    for (char c : *input) {
        if (c < '0' || c > '9') { break; }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) { return false; }
        value = value * 10 + digit;
        ++consumed;
    }
    if (consumed == 0) { return false; }
    *input = input->substr(consumed);
    *number = value;
    return true;
}

} // namespace

std::string make_filename(std::string_view dbname, uint64_t number,
                          std::string_view suffix) {
    char buf[32];
    const int len = std::snprintf(buf, sizeof(buf), "%06llu",
                                  static_cast<unsigned long long>(number));
    std::string name(buf, static_cast<size_t>(len));
    name.append(suffix.data(), suffix.size());
    return join_path(dbname, name);
}

std::string log_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".log");
}

std::string table_data_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".data");
}

std::string table_index_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".index");
}

std::string table_summary_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".summary");
}

std::string table_filter_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".filter");
}

std::string vlog_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".vlog");
}

std::string descriptor_filename(std::string_view dbname, uint64_t number) {
    char buf[32];
    const int len = std::snprintf(buf, sizeof(buf), "MANIFEST-%06llu",
                                  static_cast<unsigned long long>(number));
    std::string name(buf, static_cast<size_t>(len));
    return join_path(dbname, name);
}

std::string temp_filename(std::string_view dbname, uint64_t number) {
    return make_filename(dbname, number, ".dbtmp");
}

std::string current_filename(std::string_view dbname) {
    return join_path(dbname, "CURRENT");
}

std::string lock_filename(std::string_view dbname) {
    return join_path(dbname, "LOCK");
}

std::string infolog_filename(std::string_view dbname) {
    return join_path(dbname, "LOG");
}

std::string filename(std::string_view dbname, uint64_t number, FileType type) {
    switch (type) {
    case FileType::kLogFile: return log_filename(dbname, number);
    case FileType::kTableDataFile: return table_data_filename(dbname, number);
    case FileType::kTableIndexFile: return table_index_filename(dbname, number);
    case FileType::kTableSummaryFile:
        return table_summary_filename(dbname, number);
    case FileType::kTableFilterFile:
        return table_filter_filename(dbname, number);
    case FileType::kVLogFile: return vlog_filename(dbname, number);
    case FileType::kDescriptorFile: return descriptor_filename(dbname, number);
    case FileType::kTempFile: return temp_filename(dbname, number);
    case FileType::kCurrentFile: return current_filename(dbname);
    case FileType::kLockFile: return lock_filename(dbname);
    case FileType::kInfoLogFile: return infolog_filename(dbname);
    }
    return std::string();
}

bool parse_filename(std::string_view filename, uint64_t *number,
                    FileType *type) {
    if (number == nullptr || type == nullptr) { return false; }

    const size_t slash = filename.find_last_of('/');
    std::string_view rest = (slash == std::string_view::npos)
                                ? filename
                                : filename.substr(slash + 1);

    if (rest == "CURRENT") {
        *number = 0;
        *type = FileType::kCurrentFile;
        return true;
    }
    if (rest == "LOCK") {
        *number = 0;
        *type = FileType::kLockFile;
        return true;
    }
    if (rest == "LOG") {
        *number = 0;
        *type = FileType::kInfoLogFile;
        return true;
    }

    if (rest.starts_with("MANIFEST-")) {
        rest.remove_prefix(9);
        uint64_t num = 0;
        if (!consume_decimal_number(&rest, &num) || !rest.empty()) {
            return false;
        }
        *number = num;
        *type = FileType::kDescriptorFile;
        return true;
    }

    uint64_t num = 0;
    if (!consume_decimal_number(&rest, &num)) { return false; }
    if (rest == ".log") {
        *type = FileType::kLogFile;
    } else if (rest == ".data") {
        *type = FileType::kTableDataFile;
    } else if (rest == ".index") {
        *type = FileType::kTableIndexFile;
    } else if (rest == ".summary") {
        *type = FileType::kTableSummaryFile;
    } else if (rest == ".filter") {
        *type = FileType::kTableFilterFile;
    } else if (rest == ".vlog") {
        *type = FileType::kVLogFile;
    } else if (rest == ".dbtmp") {
        *type = FileType::kTempFile;
    } else {
        return false;
    }
    *number = num;
    return true;
}

} // namespace ANCHORDB_NAMESPACE
