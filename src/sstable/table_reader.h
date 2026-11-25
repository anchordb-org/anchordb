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
#include "anchordb/compressor.h"
#include "anchordb/env.h"
#include "anchordb/filter.h"
#include "anchordb/status.h"
#include "db/db_format.h"
#include "sstable/block_cache.h"
#include "sstable/data_block.h"
#include "sstable/filter.h"
#include "sstable/index_block.h"
#include "sstable/summary.h"
#include "sstable/table_file_set.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ANCHORDB_NAMESPACE {

class EntryIterator;

// Progress of one batched point lookup through a TableReader, in the order the
// stages run.  A staged lookup advances one stage at a time, which is what lets
// the DB layer group the same stage across many tables and run their I/O in one
// batch:
//
//   prepare_index_read()  Idle            -> IndexQueued
//   execute the batch     IndexQueued     -> IndexReady
//   prepare_data_read()   IndexReady      -> DataQueued | DataReady | NotFound | Error
//   execute the batch     DataQueued      -> DataReady
//   finish_get()          DataReady       -> Finished
//
// Finished is terminal; only a new lookup moves a finished one back to Idle.
// NotFound/Error are terminal for the lookup the stage failed on, but call()
// resets them, so they do not have to be cleared before reuse.
enum class LookUpStage : uint8_t {
    Idle = 0x0,
    IndexQueued = 0x1,
    IndexReady = 0x2,
    DataQueued = 0x3,
    DataReady = 0x4,
    NotFound = 0x5,
    Finished = 0x6,
    Error = 0x7,
};

// Storage and results for one batched point lookup.  The caller owns it and
// must keep it alive until that lookup has left every batched stage.
//
// `out` is what a get() and a get_raw() both fill, but it is only fully safe to
// read after finish_get().
//
// An inline record is exposed as a view into a pinned block or, when a reader
// bypassed the cache, into `value`.  Without a pin the caller must copy an
// inline `out.inline_value` out before it reuses this state for another lookup,
// exactly as get_raw() requires of its `scratch`.  A blob entry's handle is
// copied and needs no such care.
//
// A record served from the block cache is instead pinned by `block`, so its
// view needs no copy: it stays valid as long as this state holds the pin.  A
// new lookup on the same state releases that pin.
struct TableReadState {
    LookUpStage stage = LookUpStage::Idle;

    // What the lookup found.  For an inline record `inline_value` points into
    // the pinned block, or into `value` when nothing pins the block; in the
    // latter case it is only valid until this state is reused, so the caller
    // must copy it out first, as it must for get_raw().
    ValueRef out;
    Status status;

    // Per-lookup I/O buffers.  Only the batched path reads into them; get_raw()
    // and get() use caller-supplied storage.
    std::string index;
    std::string plain_index;
    std::string data;
    std::string plain;
    std::string value;

    // Keeps a data block resident in the block cache alive while `out`'s inline
    // view points into it, exactly as the iterator's resident pin does.  Empty
    // when the block came back from a read that bypassed the cache.
    Cache::Handle block;

    // Bytes the batcher transferred into the current buffer, checked against
    // its length once the batch has run.
    size_t bytes_read = 0;
    // The index slot this lookup selected; valid once the index block is decoded.
    size_t slot = 0;
};

// Point-read view over one SSTable.
//
// open() loads and parses the table's summary and (when configured and
// present) its filter, and keeps both resident.  A get_raw() then follows the
// read path the on-disk format prescribes:
//
//   filter -> summary -> index block -> data record (inline/tombstone)
//                                     -> ValueRef handle (blob)
//
//   1. The filter is consulted first.  A definite miss returns
//      Status::not_found without touching -Index.adb or -Data.adb.
//   2. SummaryReader maps the key to the first index block whose greatest key
//      is >= the target.
//   3. That block is read through RandomAccessFileReader, decompressed when the
//      table uses an active compressor, and parsed by IndexBlockReader.
//   4. The index slot's ValueHandle selects the payload:
//        Inline    resolve the data block through the block cache and expose
//                  the record
//        Tombstone resolve it the same way and expose the tombstone
//        Blob      return the vLog handle without reading the vLog
//
// -Data.adb blocks are read, decompressed and parsed by BlockCache, which
// retains decompressed blocks when a shared Cache is supplied.  The staged
// lookup below shares that cache: a resident block completes the lookup at
// prepare_data_read() without any read, and a block it has to read enters the
// cache.  Point reads, the staged path and iterators therefore share one
// resolution path, so a block is loaded and parsed at most once while it is
// resident.
//
// The reader performs no vLog I/O.  The DB layer resolves blob handles through
// its BlobResolver, so all tables share one resolution path and one pool of open
// vLog descriptors.
//
// A TableReader owns its table files and is non-copyable.  Its on-disk view is
// immutable after a successful open(); only the reference count changes
// afterwards.
class TableReader {
  public:
    // Opens `files` and loads the summary and filter.  `comparator`,
    // `compressor` and `filter` describe the table format and are supplied by
    // the DB/orchestrator layer from Options.  On success stores a
    // heap-allocated reader in *result; the caller owns it.  On failure returns
    // a non-ok Status and leaves *result untouched.
    // `block_cache` is a shared Cache (Options::block_cache) used to retain
    // decompressed -Data.adb blocks.  It is borrowed and must be non-null; a
    // zero-capacity cache disables retention while keeping the same ownership
    // contract.
    static Status open(Env *env, const Comparator &comparator, const Compressor &compressor, const Filter &filter,
                       const TableFileSet &files, TableReader **result, Cache *block_cache);

    ~TableReader();

    TableReader(const TableReader &) = delete;
    TableReader &operator=(const TableReader &) = delete;

    // Manual reference counting.  open() returns one reference; unref() drops
    // it and deletes the reader at zero.  Iterators and caches each hold a
    // reference so they can outlive one another safely.
    void ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void unref();

    // Point lookup.  On success fills *out with the newest stored version,
    // unresolved.  A tombstone is returned as an entry (type Tombstone), not as
    // not_found, so the DB layer can shadow older versions.  Returns
    // Status::not_found only when the key is absent from this table.  `scratch`
    // is required reusable per-call storage for the raw data record; *out's
    // inline view points into it.
    //
    // This is the whole lookup run on the calling thread: it reads the index
    // block and the data record with pread(2), one round trip each.
    Status get(std::string_view key, ValueRef *out, std::string *scratch) const;

    // Batched point lookup, staged so the DB layer can overlap the I/O of many
    // tables in one ReadBatcher submission instead of one round trip per stage
    // per table:
    //
    //   stage 1  prepare_index_read(st, key, batch)   Idle/Finished -> Index*   (queues)
    //   ...      batch->execute(), the DB layer's single submission
    //   stage 2  prepare_data_read(st, key, batch)    IndexReady -> Data*      (queues)
    //   ...      batch->execute()
    //   stage 3  finish_get(st)                       DataReady -> Finished
    //
    // Each stage performs only its own work and picks its predecessor from
    // state.stage, so the three can also be called back to back for a single
    // lookup (`call()` does exactly that).
    //
    // With `batcher` null the preparation stages read synchronously and land on
    // IndexReady/DataReady rather than the Queued stages, so the same sequence
    // still works; it just cannot overlap anything.
    //
    // IndexReady means the index bytes are in state.index and nothing more:
    // the block is parsed, the binary search runs and the selected data block
    // is resolved at the start of the next stage.  That is what keeps the record
    // view valid: the data block is only materialized once the previous stage's
    // view is no longer needed.

    // Stage 1: consults the filter, maps `key` through the summary, and queues
    // (or performs) the read of the index block that may hold it.  Leaving it at
    // IndexQueued/IndexReady is the whole effect; every later stage calls it on
    // its own if the lookup has not started.
    void prepare_index_read(TableReadState &state, std::string_view key, ReadBatcher *batcher) const;

    // Stage 2: parses the resident index block, runs the binary search, and
    // resolves the selected slot's data block.  A block already resident in the
    // cache completes the lookup here (DataReady, no I/O); otherwise the read is
    // queued (DataQueued) and joins the cache when it completes.  Returns true
    // when a data read is now queued.
    bool prepare_data_read(TableReadState &state, std::string_view key, ReadBatcher *batcher) const;

    // Stage 3: fills state.out from the selected index slot, from whichever
    // block stage 2 left in state.out.block or state.data.  Returns true on
    // success, false when state.stage is not a success state or the record does
    // not match the slot.
    bool finish_get(TableReadState &state) const;

    // Runs a whole lookup on the calling thread and leaves its outcome in
    // `state`: Finished on success, NotFound, or Error.
    void call(TableReadState &state, std::string_view key) const;

    // The batcher this reader's files read through, or null when they cannot
    // batch.  See Env::new_read_batcher().
    ReadBatcher *batcher() const;

    // Returns a heap-allocated bidirectional iterator over the whole table
    // (caller deletes).  It yields unresolved entries, tombstones included, in
    // key order; blob entries carry their handle and are not read from the
    // vLog.  The iterator holds its own reference to this reader.  An empty
    // table yields an empty iterator; a malformed table yields an error
    // iterator.
    EntryIterator *new_iterator();

    // Parity of the resident summary size for cache charging.
    size_t summary_size() const { return summary_buffer_.size(); }
    // Resident filter size, or zero when the table has no usable filter.
    size_t filter_size() const { return filter_buffer_.size(); }
    // Charge to use when caching this reader (resident summary + filter plus
    // fixed per-entry overhead).
    size_t cache_charge() const;
    // Index-block segment size recovered from the summary header.
    size_t block_size() const { return block_size_; }

  private:
    friend class TableIterator;

    explicit TableReader(Cache *block_cache) : block_cache_(block_cache) {}

    // Opens the data/index files, loads and parses the summary, and loads the
    // optional filter.
    Status init_(const TableFileSet &files);

    // Reads the index block at `block_offset` (on-disk length
    // `compressed_size`) and decompresses it when needed.  *out points into
    // *stored_buf or *plain_buf, whichever holds the parseable block.
    Status read_index_block_(uint64_t block_offset, uint32_t compressed_size, std::string *stored_buf,
                             std::string *plain_buf, std::string_view *out) const;

    // `read_index_block_` once the on-disk bytes are already resident, turning
    // `stored` into the parseable block in *out.
    Status decode_index_block_(std::string_view stored, std::string *plain_buf, std::string_view *out) const;

    // Batched-path primitives.  queue_*_read_ stages a read on `batcher` (or
    // performs it synchronously when it is null) and records the resulting
    // stage, so the error/short-read handling lives in one place per buffer.
    void queue_index_read_(const SummaryEntry &entry, TableReadState *state, ReadBatcher *batcher,
                           bool *done = nullptr) const;
    void queue_data_read_(const ValueHandle &handle, TableReadState *state, ReadBatcher *batcher) const;
    // This table's block cache key for `handle`'s data block.  Returned by
    // value: it must outlive the cache call that consumes it.
    std::string block_cache_key_(const ValueHandle &handle) const;
    // Pins the data block `handle` selects from `reader`'s cache when it is
    // resident, so a lookup can complete without a read at prepare_data_read().
    // On a miss state.block is left empty and the caller reads the block.
    bool lookup_resident_block_(const ValueHandle &handle, TableReadState *state) const;
    // Publishes the block `handle` addresses, read into state.data, inside the
    // cache unless a concurrent lookup already put it there.  Either way
    // state.block ends up pinning the cache slot, so state.data is only a
    // transport buffer from here on.
    Status adopt_data_block_(const ValueHandle &handle, TableReadState *state) const;
    // Decodes `handle`'s stored data block from state.data into state.plain, for
    // a block this reader could not adopt into the cache.
    // Decodes `handle`'s block from the state's read buffer into state.plain,
    // for a block this reader could not adopt into the cache.  Measured by the
    // byte count the read transferred.
    Status decode_data_block_(const ValueHandle &handle, TableReadState *state) const;
    // Finds the record `handle` addresses inside `block` and validates it
    // against `expected_key`.  Returns the entry by value; its views point into
    // `block`, not into the short-lived parser.
    Status find_record_(const ValueHandle &handle, std::string_view block, std::string_view expected_key,
                        DataBlockEntry *record_out) const;
    // Exposes the selected inline record's value in state.out, copying into
    // state.value when the block is not pinned by state.block.
    Status read_inline_value_from_state_(const IndexBlockEntry &entry, TableReadState *state) const;

    // Resolves an inline/tombstone slot through the block cache: loads and
    // parses its data block, finds the record at handle.data_offset, and
    // validates its key and type.  On success `*pin` owns the block and
    // `*entry_out` points into it; the caller must not use either after the pin
    // is released.
    Status resolve_inline_(const ValueHandle &handle, std::string_view expected_key, Cache::Handle *pin,
                           const DataBlockEntry **entry_out) const;

    // Reads and validates the inline/tombstone data record selected by
    // `handle`, exposing its value bytes through *value.  The view points into
    // `scratch`.
    Status read_inline_value_(const ValueHandle &handle, std::string_view expected_key, std::string *scratch,
                              std::string_view *value) const;

    // Fills *out from one index slot: inline/tombstone values are read into
    // `scratch`, blob entries are returned as a handle with no vLog read.  Single
    // implementation shared by get_raw() and the table iterator.
    Status make_value_ref_(const IndexBlockEntry &entry, std::string *scratch, ValueRef *out) const;

    // Reads the filter first, so a definite miss ends the lookup without any
    // index or data I/O.  On false the lookup is already NotFound or Error.
    bool filter_admits_(TableReadState &state, std::string_view key) const;
    // Resolves the summary block for `key` and queues its index block into
    // state.index, or records NotFound when the key is past the last block.
    void prepare_index_(TableReadState &state, std::string_view key, ReadBatcher *batcher) const;
    // Validates the queued index read, parses the block and records the slot.
    bool decode_index_(TableReadState *state, std::string_view key) const;
    // Binary-searches the parsed block for `key` and queues the selected slot's
    // data record when it has one.
    bool select_and_prepare_data_(TableReadState *state, ReadBatcher *batcher) const;
    // Turns the resolved data block into state.out for the lookup's selected
    // slot.
    bool resolve_and_finish_(TableReadState *state) const;

    std::atomic<int> refs_{0};

    Comparator comparator_;
    Compressor compressor_;
    Filter filter_spec_;
    Env *env_ = nullptr;

    // Resident summary and its parser.  summary_buffer_ must outlive summary_.
    std::string summary_buffer_;
    SummaryReader *summary_ = nullptr;

    // Resident filter and its parser.  Empty/unparseable filters leave filter_
    // null, which makes every lookup consult the index.
    std::string filter_buffer_;
    FilterReader *filter_ = nullptr;

    RandomAccessFileReader *index_reader_ = nullptr;
    RandomAccessFileReader *data_reader_ = nullptr;
    uint64_t index_file_size_ = 0;
    uint64_t data_file_size_ = 0;
    size_t block_size_ = 0;

    // Codec for framed -Data.adb blocks, recovered from the summary.
    Compressor data_compressor_;

    // Shared (borrowed) data-block cache.
    BlockCache block_cache_;
};

} // namespace ANCHORDB_NAMESPACE
