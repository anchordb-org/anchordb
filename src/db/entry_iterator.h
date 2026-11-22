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
#include "anchordb/status.h"
#include "db/db_format.h"
#include <cstdint>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// Bidirectional iterator over one storage source (a memtable or one SSTable).
//
// Unlike the public Iterator, an EntryIterator yields an unresolved entry: it
// reports the version's type and timestamp cheaply from the source's own
// metadata, and its value_ref() returns either an inline view or a BlobHandle.
// It never performs vLog I/O, and it does NOT hide tombstones: the DB layer
// needs to see them to decide which version is newest.
//
// Position-dependent views are valid only until the iterator is repositioned or
// destroyed.  Not thread-safe.
class EntryIterator {
  public:
    EntryIterator() = default;
    EntryIterator(const EntryIterator &) = delete;
    EntryIterator &operator=(const EntryIterator &) = delete;
    virtual ~EntryIterator() = default;

    virtual bool valid() const = 0;
    virtual void seek_to_first() = 0;
    virtual void seek_to_last() = 0;
    // Positions at the first key >= target, or invalidates if there is none.
    virtual void seek(std::string_view target) = 0;
    virtual void next() = 0;
    virtual void prev() = 0;

    // Current entry's user key.  REQUIRES: valid().
    virtual std::string_view key() const = 0;
    // Current entry's type.  Cheap: reads no data or vLog record.  REQUIRES:
    // valid().
    virtual EntryType type() const = 0;
    // Current entry's timestamp.  Cheap, like type().  REQUIRES: valid().
    virtual uint64_t timestamp() const = 0;
    // Current entry's value.  For Inline/Tombstone this reads the data record;
    // for Blob it returns the handle and performs no vLog I/O.  REQUIRES:
    // valid().
    virtual ValueRef value_ref() const = 0;

    virtual Status status() const = 0;
};

// An always-invalid iterator with a success status.
EntryIterator *new_empty_entry_iterator();
// An always-invalid iterator that reports `status` from status().
EntryIterator *new_error_entry_iterator(const Status &status);

} // namespace ANCHORDB_NAMESPACE
