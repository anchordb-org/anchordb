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

#include "sstable/data_block.h"
#include "sstable/data_record.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

DataBlockBuilder::DataBlockBuilder(size_t target_size)
    : target_size_(target_size == 0 ? kDefaultDataBlockSize : std::min(target_size, kMaxDataBlockSize)) {}

DataBlockBuilder::AddResult DataBlockBuilder::add(std::string_view key, std::string_view value, EntryType type,
                                                  uint64_t timestamp, uint32_t *in_block_offset) {
    if (sealed_) { return AddResult::Full; }
    if (type != EntryType::Inline && type != EntryType::Tombstone) { return AddResult::Invalid; }
    if (key.size() > kDataRecordMaxKeySize) { return AddResult::Invalid; }

    const size_t record_len = data_record_encoded_len(key.size(), value.size());
    if (record_len > kMaxDataBlockSize) { return AddResult::Invalid; }
    if (!buffer_.empty() && buffer_.size() + record_len > target_size_) { return AddResult::Full; }
    if (buffer_.size() + record_len > kMaxDataBlockSize) { return AddResult::Invalid; }

    if (in_block_offset != nullptr) { *in_block_offset = static_cast<uint32_t>(buffer_.size()); }
    encode_data_record(&buffer_, timestamp, key, value, type);
    ++record_count_;
    return AddResult::Added;
}

std::string_view DataBlockBuilder::finish() {
    sealed_ = true;
    return std::string_view(buffer_);
}

void DataBlockBuilder::reset() {
    buffer_.clear();
    record_count_ = 0;
    sealed_ = false;
}

DataBlockReader::DataBlockReader(std::string_view block) : valid_(true) {
    std::string_view cursor = block;
    while (!cursor.empty()) {
        const uint32_t offset = static_cast<uint32_t>(block.size() - cursor.size());
        ParsedDataRecord record;
        Status s = decode_data_record(&cursor, &record);
        if (!s.is_ok()) {
            valid_ = false;
            break;
        }
        if (record.type != EntryType::Inline && record.type != EntryType::Tombstone) {
            valid_ = false;
            break;
        }
        DataBlockEntry entry;
        entry.key = record.key;
        entry.value = record.value;
        entry.type = record.type;
        entry.timestamp = record.timestamp;
        entry.in_block_offset = offset;
        entries_.push_back(entry);
    }

    if (!valid_) { entries_.clear(); }
}

size_t DataBlockReader::lower_bound(std::string_view target, const Comparator &cmp) const {
    if (!valid_) { return entries_.size(); }
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), target,
        [&cmp](const DataBlockEntry &entry, std::string_view key) { return cmp.compare(entry.key, key) < 0; });
    return static_cast<size_t>(it - entries_.begin());
}

bool DataBlockReader::find_offset(uint32_t in_block_offset, size_t *index) const {
    const auto it =
        std::lower_bound(entries_.begin(), entries_.end(), in_block_offset,
                         [](const DataBlockEntry &entry, uint32_t offset) { return entry.in_block_offset < offset; });
    if (it == entries_.end() || it->in_block_offset != in_block_offset) { return false; }
    if (index != nullptr) { *index = static_cast<size_t>(it - entries_.begin()); }
    return true;
}

namespace {

class DataBlockIter : public Iterator {
  public:
    DataBlockIter(const DataBlockReader &reader, Comparator comparator)
        : reader_(reader), comparator_(comparator), index_(reader.size()) {}

    bool valid() const override { return index_ < reader_.size(); }

    void seek_to_first() override { index_ = reader_.empty() ? reader_.size() : 0; }

    void seek_to_last() override { index_ = reader_.empty() ? reader_.size() : reader_.size() - 1; }

    void seek(std::string_view target) override { index_ = reader_.lower_bound(target, comparator_); }

    void next() override {
        assert(valid());
        ++index_;
    }

    void prev() override {
        assert(valid());
        index_ = (index_ == 0) ? reader_.size() : index_ - 1;
    }

    std::string_view key() const override {
        assert(valid());
        return reader_.entry(index_).key;
    }

    std::string_view value() const override {
        assert(valid());
        return reader_.entry(index_).value;
    }

    Status status() const override { return Status::ok(); }

  private:
    const DataBlockReader &reader_;
    const Comparator comparator_;
    size_t index_;
};

} // namespace

Iterator *DataBlockReader::new_iterator(Comparator comparator) const {
    if (!valid_) { return new_error_iterator(Status::corruption("bad data block contents")); }
    if (empty()) { return new_empty_iterator(); }
    if (comparator.compare == nullptr) { comparator = new_byte_wise_comparator(); }
    return new DataBlockIter(*this, comparator);
}

} // namespace ANCHORDB_NAMESPACE
