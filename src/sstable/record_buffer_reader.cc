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

#include "sstable/record_buffer_reader.h"
#include "sstable/data_record.h"

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

Status RecordBufferReader::next(ParsedDataRecord *result, bool *done) {
    if (result == nullptr || done == nullptr) {
        return Status::invalid_argument("record_buffer_reader: null argument");
    }
    *result = ParsedDataRecord{};
    *done = false;

    if (pos_ >= buffer_.size()) {
        *done = true;
        return Status::ok();
    }

    std::string_view cursor = buffer_.substr(pos_);
    Status s = decode_data_record(&cursor, result);
    if (!s.is_ok()) { return s; }

    pos_ = buffer_.size() - cursor.size();
    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
