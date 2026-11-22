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
#include "anchordb/iterator.h"
#include "anchordb/status.h"
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class BlobResolver;
class EntryIterator;

// The public, byte-valued iterator, produced by the DB layer for one source.
//
// It wraps an EntryIterator (a memtable or one SSTable) and presents at most one
// visible entry per user key: it keeps the newest version and drops older
// versions of the same key, and it drops a key entirely when its newest version
// is a tombstone.  It resolves a blob through BlobResolver only when value() is
// called; positioning and version filtering never read the vLog.
//
// This is single-source visibility only.  Collapsing the same key across
// several sources (memtable + tables) still requires the DB merge layer, which
// must run before or instead of this iterator.  `comparator` is the user-key
// comparator the wrapped iterator is ordered by.
//
// Takes ownership of `inner`; `resolver` is borrowed and must outlive this
// iterator.
class ResolvingIterator final : public Iterator {
  public:
    ResolvingIterator(EntryIterator *inner, BlobResolver *resolver, Comparator comparator);
    ~ResolvingIterator() override;

    ResolvingIterator(const ResolvingIterator &) = delete;
    ResolvingIterator &operator=(const ResolvingIterator &) = delete;

    bool valid() const override { return valid_; }
    void seek_to_first() override;
    void seek_to_last() override;
    void seek(std::string_view target) override;
    void next() override;
    void prev() override;
    std::string_view key() const override;
    std::string_view value() const override;
    Status status() const override;

  private:
    // Positions on the newest version of the next visible key, or invalidates.
    void settle_forward();
    // Positions on the newest version of the previous visible key, or
    // invalidates.
    void settle_backward();
    bool same_key(std::string_view a, std::string_view b) const;
    void reset_value() const;

    EntryIterator *inner_;
    BlobResolver *resolver_;
    Comparator comparator_;
    mutable bool valid_ = false;

    mutable std::string value_buf_;
    // Reusable staging buffer for the raw vLog record during blob resolution.
    mutable std::string blob_scratch_;
    mutable std::string_view value_view_;
    mutable bool value_ready_ = false;
    mutable Status status_;
};

// Convenience factory.  Returns an error iterator when `inner` is null.  A null
// `comparator` falls back to byte-wise ordering.
Iterator *new_resolving_iterator(EntryIterator *inner, BlobResolver *resolver, Comparator comparator = Comparator{});

} // namespace ANCHORDB_NAMESPACE
