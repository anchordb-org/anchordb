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

#include "db/entry_iterator.h"

#include <cstdint>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Always-invalid iterator carrying a fixed status.  Backs both the empty and the
// error factories.
class EmptyEntryIterator final : public EntryIterator {
  public:
    explicit EmptyEntryIterator(Status status) : status_(std::move(status)) {}

    bool valid() const override { return false; }
    void seek_to_first() override {}
    void seek_to_last() override {}
    void seek(std::string_view) override {}
    void next() override {}
    void prev() override {}

    std::string_view key() const override { return std::string_view(); }
    EntryType type() const override { return EntryType::Inline; }
    uint64_t timestamp() const override { return 0; }
    ValueRef value_ref() const override { return ValueRef(); }
    Status status() const override { return status_; }

  private:
    Status status_;
};

} // namespace

EntryIterator *new_empty_entry_iterator() { return new EmptyEntryIterator(Status::ok()); }

EntryIterator *new_error_entry_iterator(const Status &status) { return new EmptyEntryIterator(status); }

} // namespace ANCHORDB_NAMESPACE
