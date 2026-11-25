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

class ReadBatcher;

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

    // Staged seek, mirroring TableReader's staged point lookup so the DB layer
    // can overlap the seek I/O of many sources in one ReadBatcher submission:
    //
    //   prepare_seek_index(target, batch)  queues the first index-block read
    //   ...                                batch->execute()
    //   prepare_seek_data(batch)           positions and queues the data read
    //   ...                                batch->execute()
    //   finish_seek()                      pins the landed entry's data block
    //
    // After finish_seek() a successful seek leaves the iterator positioned
    // (valid()) and, for an inline/tombstone entry, pins its data block, so a
    // following value_ref() performs no I/O.  An empty result leaves the
    // iterator invalid with a success status.  Sources without batching may do
    // the whole seek in prepare_seek_index() and treat the other stages as
    // no-ops, which is what the defaults below do.
    virtual void prepare_seek_index(std::string_view target, ReadBatcher *batcher) {
        (void)batcher;
        seek(target);
    }
    // Returns true when a data read was queued and the caller must execute the
    // batch before finish_seek().  False when the seek is already resolved.
    virtual bool prepare_seek_data(ReadBatcher *batcher) {
        (void)batcher;
        return false;
    }
    // Finalizes the staged seek.  Returns true when the seek completed (the
    // iterator may still be invalid for an out-of-range target); false on error.
    virtual bool finish_seek() { return status().is_ok(); }

    // Sequential read-ahead: prefetch the next index block so the block
    // boundary a merge scan eventually crosses is already in memory.  Adds one
    // operation per source to `batcher`; no-op when there is no next block, no
    // batcher, or one is already in flight.  The caller submits the batch and
    // later calls finish_readahead(); repositioning an iterator while a prefetch
    // is in flight is not allowed.
    //
    // Merge driver loop, per tick:
    //   if (batcher->poll()) { for each it: it->finish_readahead();
    //                          for each it: it->prepare_readahead(batcher);
    //                          batcher->submit(); }
    // readahead keeps at most one generation in flight, across all sources.
    virtual void prepare_readahead(ReadBatcher *batcher) { (void)batcher; }
    // Adopts a prefetched index block once the batch containing it has completed
    // (or drops it if it failed).  Cheap; call after poll() reports completion.
    virtual void finish_readahead() {}
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
