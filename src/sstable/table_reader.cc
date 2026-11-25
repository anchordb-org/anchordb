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

#include "sstable/table_reader.h"
#include "anchordb/env.h"
#include "db/entry_iterator.h"
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Upper bound on a summary or filter file we are willing to load into memory.
constexpr uint64_t kMaxSummaryFileSize = 64ull * 1024 * 1024;
constexpr uint64_t kMaxFilterFileSize = 64ull * 1024 * 1024;

// Fixed byte overhead charged per cached reader, on top of the resident
// summary and filter bytes.
constexpr size_t kTableCacheEntryOverhead = 256;

// Reads exactly `n` bytes at `offset` into *buf and points *out at them.
Status read_exact(RandomAccessFileReader *reader, uint64_t offset, size_t n, std::string *buf, std::string_view *out) {
    *out = std::string_view();
    if (n == 0) {
        buf->clear();
        return Status::ok();
    }

    buf->resize(n);
    std::string_view view;
    Status s = reader->read(offset, n, &view, buf->data());
    if (!s.is_ok()) { return s; }
    if (view.size() < n) { return Status::corruption("table reader: short read", std::to_string(view.size())); }
    if (view.data() != buf->data()) { std::memmove(buf->data(), view.data(), n); }
    *out = std::string_view(buf->data(), n);
    return Status::ok();
}

} // namespace

TableReader::~TableReader() {
    delete summary_;
    delete filter_;
    delete index_reader_;
    delete data_reader_;
}

void TableReader::unref() {
    const int old = refs_.fetch_sub(1, std::memory_order_acq_rel);
    assert(old > 0);
    if (old == 1) { delete this; }
}

size_t TableReader::cache_charge() const {
    return summary_buffer_.size() + filter_buffer_.size() + kTableCacheEntryOverhead;
}

Status TableReader::open(Env *env, const Comparator &comparator, const Compressor &compressor, const Filter &filter,
                         const TableFileSet &files, TableReader **result, Cache *block_cache) {
    if (result == nullptr) { return Status::invalid_argument("TableReader::open: null result"); }
    if (env == nullptr) { return Status::invalid_argument("TableReader::open: env is null"); }
    if (block_cache == nullptr) { return Status::invalid_argument("TableReader::open: block cache is null"); }

    TableReader *reader = new TableReader(block_cache);
    reader->env_ = env;
    reader->comparator_ = comparator;
    if (reader->comparator_.compare == nullptr) { reader->comparator_ = new_byte_wise_comparator(); }
    reader->compressor_ = compressor;
    reader->filter_spec_ = filter;

    Status s = reader->init_(files);
    if (!s.is_ok()) {
        delete reader;
        return s;
    }

    reader->refs_.store(1, std::memory_order_relaxed);
    *result = reader;
    return Status::ok();
}

Status TableReader::init_(const TableFileSet &files) {
    if (files.data.empty() || files.index.empty() || files.summary.empty()) {
        return Status::invalid_argument("TableReader: missing data, index or summary file name");
    }

    uint64_t summary_size = 0;
    uint64_t index_size = 0;
    uint64_t data_size = 0;
    Status s = env_->get_file_size(files.summary, &summary_size);
    if (!s.is_ok()) { return s; }
    s = env_->get_file_size(files.index, &index_size);
    if (!s.is_ok()) { return s; }
    s = env_->get_file_size(files.data, &data_size);
    if (!s.is_ok()) { return s; }
    if (summary_size > kMaxSummaryFileSize) {
        return Status::corruption("table reader: summary file too large", std::to_string(summary_size));
    }

    s = env_->new_random_access_file_reader(files.index, &index_reader_);
    if (!s.is_ok()) { return s; }
    s = env_->new_random_access_file_reader(files.data, &data_reader_);
    if (!s.is_ok()) { return s; }
    index_file_size_ = index_size;
    data_file_size_ = data_size;

    RandomAccessFileReader *summary_reader = nullptr;
    s = env_->new_random_access_file_reader(files.summary, &summary_reader);
    if (!s.is_ok()) { return s; }
    std::string_view summary_view;
    s = read_exact(summary_reader, 0, static_cast<size_t>(summary_size), &summary_buffer_, &summary_view);
    delete summary_reader;
    if (!s.is_ok()) { return s; }

    summary_ = new SummaryReader(summary_buffer_, comparator_);
    if (!summary_->valid()) { return Status::corruption("table reader: invalid summary index"); }

    // The summary records which codec compressed the index blocks.  Reading a
    // table with any other codec (or with compression disabled) would silently
    // misinterpret the stored bytes, so reject it up front.
    const uint32_t stored_compressor_id = summary_->compressor_id();
    if (stored_compressor_id != compressor_.id) {
        return Status::corruption("table reader: compressor id mismatch",
                                  std::to_string(stored_compressor_id) + " != " + std::to_string(compressor_.id));
    }
    if (stored_compressor_id != 0 &&
        (compressor_.uncompress == nullptr || compressor_.get_uncompressed_length == nullptr)) {
        return Status::corruption("table reader: compressor unavailable for table");
    }

    // The summary header pins the index-block segment size for this table, and
    // its entries are the source of truth for the block layout.  Validate that
    // every entry maps inside -Index.adb before any read uses it.
    block_size_ = summary_->block_size();
    if (block_size_ == 0) { return Status::corruption("table reader: zero index block size"); }

    // Every inline slot addresses a framed -Data.adb block:
    // (block offset, stored block size, in-block record offset).
    const uint32_t data_block_size = summary_->data_block_size();
    if (data_block_size == 0 || data_block_size > kMaxDataBlockSize) {
        return Status::corruption("table reader: invalid data block size", std::to_string(data_block_size));
    }
    const uint32_t data_compressor_id = summary_->data_compressor_id();
    data_compressor_ = new_compressor_by_id(data_compressor_id);
    if (data_compressor_id != 0 &&
        (data_compressor_.uncompress == nullptr || data_compressor_.get_uncompressed_length == nullptr)) {
        return Status::corruption("table reader: data compressor unavailable", std::to_string(data_compressor_id));
    }

    for (size_t i = 0; i < summary_->size(); ++i) {
        const SummaryEntry &entry = summary_->entry(i);
        if (entry.compressed_size == 0 || entry.block_offset > index_size ||
            entry.compressed_size > index_size - entry.block_offset) {
            return Status::corruption("table reader: summary entry out of range", std::to_string(entry.block_offset));
        }
    }

    // The filter is optional: a missing or unreadable filter simply disables
    // filtering instead of failing the open.
    const bool filter_configured = filter_spec_.key_may_match != nullptr && filter_spec_.name != nullptr;
    if (filter_configured && !files.filter.empty()) {
        uint64_t filter_size = 0;
        Status fs = env_->get_file_size(files.filter, &filter_size);
        if (fs.is_ok() && filter_size > 0) {
            if (filter_size > kMaxFilterFileSize) {
                return Status::corruption("table reader: filter file too large", std::to_string(filter_size));
            }
            RandomAccessFileReader *filter_reader = nullptr;
            fs = env_->new_random_access_file_reader(files.filter, &filter_reader);
            if (!fs.is_ok()) { return fs; }
            std::string_view filter_view;
            fs = read_exact(filter_reader, 0, static_cast<size_t>(filter_size), &filter_buffer_, &filter_view);
            delete filter_reader;
            if (!fs.is_ok()) { return fs; }

            FilterReader *parsed = new FilterReader(filter_spec_, filter_buffer_);
            if (parsed->valid()) {
                filter_ = parsed;
            } else {
                delete parsed;
            }
        }
    }

    return Status::ok();
}

Status TableReader::get(std::string_view key, ValueRef *out, std::string *scratch) const {
    if (out == nullptr) { return Status::invalid_argument("TableReader::get_raw: null value ref"); }
    if (scratch == nullptr) { return Status::invalid_argument("TableReader::get_raw: null scratch"); }

    // A definite filter miss means the key is absent, so skip all I/O.
    if (filter_ != nullptr && !filter_->key_may_match(key)) {
        return Status::not_found("key not in table filter", std::string(key));
    }

    const size_t block_index = summary_->lower_bound(key);
    if (block_index >= summary_->size()) { return Status::not_found("key not in table range", std::string(key)); }
    const SummaryEntry &entry = summary_->entry(block_index);

    std::string stored_buf;
    std::string plain_buf;
    std::string_view block;
    Status s = read_index_block_(entry.block_offset, entry.compressed_size, &stored_buf, &plain_buf, &block);
    if (!s.is_ok()) { return s; }

    IndexBlockReader reader(block, block_size_);
    if (!reader.valid()) { return Status::corruption("table reader: invalid index block"); }

    const size_t slot = reader.lower_bound(key, comparator_);
    if (slot >= reader.size() || comparator_.compare(reader.key(slot), key) != 0) {
        return Status::not_found("key not found", std::string(key));
    }

    return make_value_ref_(reader.entry(slot), scratch, out);
}

// Bidirectional iterator over a TableReader's index.  It walks index slots in
// key order and spans block boundaries, but never reads a data record or a vLog
// record just to position: type()/timestamp() come from the resident index slot,
// and value_ref() lazily reads the data record only for inline and tombstone
// entries.  Blob entries yield their handle untouched.
class TableIterator final : public EntryIterator {
  public:
    TableIterator(TableReader *owner)
        : owner_(owner), comparator_(owner->comparator_), block_count_(owner->summary_->size()) {
        owner_->ref();
    }

    ~TableIterator() override { owner_->unref(); }

    TableIterator(const TableIterator &) = delete;
    TableIterator &operator=(const TableIterator &) = delete;

    bool valid() const override { return valid_; }

    void seek_to_first() override {
        status_ = Status::ok();
        valid_ = false;
        clear_seek_prefetch();
        clear_readahead();
        if (block_count_ == 0) {
            invalidate();
            return;
        }
        load_block(0);
        if (!status_.is_ok()) { return; }
        slot_ = 0;
        settle_forward();
    }

    void seek_to_last() override {
        status_ = Status::ok();
        valid_ = false;
        clear_seek_prefetch();
        clear_readahead();
        if (block_count_ == 0) {
            invalidate();
            return;
        }
        load_block(block_count_ - 1);
        if (!status_.is_ok()) { return; }
        slot_ = block_reader_->size();
        settle_backward();
    }

    void seek(std::string_view target) override {
        status_ = Status::ok();
        valid_ = false;
        clear_seek_prefetch();
        clear_readahead();
        if (block_count_ == 0) {
            invalidate();
            return;
        }

        const size_t block_index = owner_->summary_->lower_bound(target);
        if (block_index >= owner_->summary_->size()) {
            invalidate();
            return;
        }
        load_block(block_index);
        if (!status_.is_ok()) { return; }
        slot_ = block_reader_->lower_bound(target, comparator_);
        settle_forward();
    }

    // Staged seek: same three stages as TableReader's staged point lookup, so a
    // DB-level iterator can batch the index (and then data) reads of many
    // sources.  prepare_seek_index() queues the index read, prepare_seek_data()
    // positions and queues the landed entry's data block, and finish_seek()
    // pins that block so value_ref() needs no I/O.
    void prepare_seek_index(std::string_view target, ReadBatcher *batcher) override {
        status_ = Status::ok();
        valid_ = false;
        clear_seek_prefetch();
        clear_readahead();
        seek_target_.assign(target.data(), target.size());

        delete block_reader_;
        block_reader_ = nullptr;

        seek_read_.stage = LookUpStage::Idle;
        seek_read_.slot = 0;
        seek_read_.status = Status::ok();

        if (block_count_ == 0) {
            seek_read_.stage = LookUpStage::NotFound;
            return;
        }
        const size_t block_index = owner_->summary_->lower_bound(seek_target_);
        if (block_index >= owner_->summary_->size()) {
            seek_read_.stage = LookUpStage::NotFound;
            return;
        }
        block_index_ = block_index;
        owner_->queue_index_read_(owner_->summary_->entry(block_index), &seek_read_, batcher);
        if (seek_read_.stage == LookUpStage::Error) { status_ = seek_read_.status; }
    }

    bool prepare_seek_data(ReadBatcher *batcher) override {
        // Nothing to resolve when the index stage already finished the seek or
        // failed; a caller batching many sources may still call this stage.
        if (seek_read_.stage == LookUpStage::NotFound || seek_read_.stage == LookUpStage::Finished ||
            seek_read_.stage == LookUpStage::Error) {
            return false;
        }
        if (!decode_seek_index_()) { return false; }

        slot_ = block_reader_->lower_bound(seek_target_, comparator_);
        settle_forward();
        if (!valid_) {
            seek_read_.status = status_;
            seek_read_.stage = status_.is_ok() ? LookUpStage::NotFound : LookUpStage::Error;
            return false;
        }

        seek_read_.slot = slot_;
        const IndexBlockEntry entry = block_reader_->entry(slot_);
        if (is_vlog_blob_type(entry.handle.type)) {
            seek_read_.stage = LookUpStage::Finished;
            return false;
        }

        // A resident block needs no read; finish_seek() turns the pin it leaves
        // in seek_read_.block into the iterator's prefetched block.
        if (owner_->lookup_resident_block_(entry.handle, &seek_read_)) {
            seek_read_.stage = LookUpStage::DataReady;
            return false;
        }

        owner_->queue_data_read_(entry.handle, &seek_read_, batcher);
        if (seek_read_.stage == LookUpStage::Error) { status_ = seek_read_.status; }
        return seek_read_.stage == LookUpStage::DataQueued;
    }

    bool finish_seek() override {
        if (seek_read_.stage == LookUpStage::Error) { return false; }
        if (seek_read_.stage == LookUpStage::NotFound || seek_read_.stage == LookUpStage::Finished) {
            return status_.is_ok();
        }

        if (seek_read_.stage == LookUpStage::DataQueued) {
            const IndexBlockEntry entry = block_reader_->entry(slot_);
            if (!seek_read_.status.is_ok() || seek_read_.bytes_read != seek_read_.data.size()) {
                status_ = seek_read_.status.is_ok() ? Status::corruption("table iterator: short data block read",
                                                                         std::to_string(seek_read_.bytes_read))
                                                    : seek_read_.status;
                seek_read_.stage = LookUpStage::Error;
                return false;
            }
            Status s = owner_->adopt_data_block_(entry.handle, &seek_read_);
            if (!s.is_ok()) {
                status_ = s;
                seek_read_.stage = LookUpStage::Error;
                return false;
            }
            seek_read_.stage = LookUpStage::DataReady;
        }

        if (seek_read_.stage != LookUpStage::DataReady) {
            status_ = Status::corruption("table iterator: finish_seek without a resolved seek");
            seek_read_.stage = LookUpStage::Error;
            return false;
        }

        resident_data_block_ = std::move(seek_read_.block);
        seek_pinned_block_ = block_index_;
        seek_pinned_slot_ = slot_;
        seek_data_pinned_ = true;
        status_ = Status::ok();
        seek_read_.stage = LookUpStage::Finished;
        return true;
    }

    void prepare_readahead(ReadBatcher *batcher) override {
        if (batcher == nullptr || block_reader_ == nullptr) { return; }
        if (readahead_in_flight_ || readahead_reader_ != nullptr) { return; }
        const size_t next = block_index_ + 1;
        if (next >= block_count_) { return; }

        readahead_state_.stage = LookUpStage::Idle;
        readahead_state_.slot = 0;
        readahead_state_.status = Status::ok();
        readahead_state_.bytes_read = 0;
        readahead_block_ = next;
        readahead_done_ = false;
        readahead_cancelled_ = false;

        owner_->queue_index_read_(owner_->summary_->entry(next), &readahead_state_, batcher, &readahead_done_);
        if (readahead_state_.stage == LookUpStage::IndexQueued) {
            readahead_in_flight_ = true;
            return;
        }
        // The reader could not batch and read synchronously; decode now.
        readahead_done_ = true;
        install_readahead_();
    }

    void finish_readahead() override {
        if (!readahead_in_flight_ || !readahead_done_) { return; }
        readahead_in_flight_ = false;
        install_readahead_();
    }

    void next() override {
        assert(valid_);
        clear_seek_prefetch();
        ++slot_;
        settle_forward();
    }

    void prev() override {
        assert(valid_);
        clear_seek_prefetch();
        clear_readahead();
        if (slot_ > 0) {
            --slot_;
            valid_ = true;
            return;
        }
        if (block_index_ == 0) {
            invalidate();
            return;
        }
        load_block(block_index_ - 1);
        if (!status_.is_ok()) {
            invalidate();
            return;
        }
        slot_ = block_reader_->size();
        settle_backward();
    }

    std::string_view key() const override {
        assert(valid_);
        return block_reader_->entry(slot_).key;
    }

    EntryType type() const override {
        assert(valid_);
        return block_reader_->entry(slot_).handle.type;
    }

    uint64_t timestamp() const override {
        assert(valid_);
        return block_reader_->entry(slot_).timestamp;
    }

    ValueRef value_ref() const override {
        assert(valid_);
        const IndexBlockEntry entry = block_reader_->entry(slot_);

        // A staged seek already read and pinned this entry's data block, so
        // resolve it straight from the pin instead of looking the block up.
        if (!is_vlog_blob_type(entry.handle.type) && seek_data_pinned_ && seek_pinned_block_ == block_index_ &&
            seek_pinned_slot_ == slot_) {
            const auto *block = static_cast<CachedDataBlock *>(resident_data_block_.value());
            if (block == nullptr) {
                status_ = Status::corruption("table iterator: prefetched block missing");
                return ValueRef();
            }
            DataBlockEntry record;
            Status s = owner_->find_record_(entry.handle, block->bytes, entry.key, &record);
            if (!s.is_ok()) {
                status_ = s;
                return ValueRef();
            }
            ValueRef ref;
            ref.type = record.type;
            ref.timestamp = entry.timestamp;
            ref.inline_value = record.value;
            return ref;
        }

        // Resolve inline/tombstone records through the block cache.  The pin
        // keeps the block alive until the next call, so record views stay valid
        // across repeated reads of the same entry.
        if (!is_vlog_blob_type(entry.handle.type)) {
            Cache::Handle pin;
            const DataBlockEntry *record = nullptr;
            Status s = owner_->resolve_inline_(entry.handle, entry.key, &pin, &record);
            if (!s.is_ok()) {
                status_ = s;
                return ValueRef();
            }
            resident_data_block_ = std::move(pin);
            ValueRef ref;
            ref.type = record->type;
            ref.timestamp = entry.timestamp;
            ref.inline_value = record->value;
            return ref;
        }

        ValueRef ref;
        Status s = owner_->make_value_ref_(entry, &scratch_, &ref);
        if (!s.is_ok()) {
            status_ = s;
            return ValueRef();
        }
        return ref;
    }

    Status status() const override { return status_; }

  private:
    // Releases the block a staged seek pinned, so a subsequent reposition cannot
    // return a view into the wrong entry's bytes.
    void clear_seek_prefetch() {
        seek_data_pinned_ = false;
        resident_data_block_.reset();
    }

    // Turns a queued or synchronous index read into a heap reader that borrows
    // `state`'s buffers.  Returns null (with *s) when the block is unusable.
    Status decode_index_state_(TableReadState &state, IndexBlockReader **reader_out) {
        *reader_out = nullptr;
        if (state.stage == LookUpStage::IndexQueued) {
            if (!state.status.is_ok()) { return state.status; }
        } else if (state.stage != LookUpStage::IndexReady) {
            return Status::corruption("table iterator: decode without an index block");
        }

        std::string_view block;
        Status s = owner_->decode_index_block_(std::string_view(state.index).substr(0, state.bytes_read),
                                               &state.plain_index, &block);
        if (!s.is_ok()) { return s; }

        auto *reader = new IndexBlockReader(block, owner_->block_size_);
        if (!reader->valid()) {
            delete reader;
            return Status::corruption("invalid index block");
        }
        *reader_out = reader;
        return Status::ok();
    }

    bool decode_seek_index_() {
        IndexBlockReader *reader = nullptr;
        Status s = decode_index_state_(seek_read_, &reader);
        if (!s.is_ok()) {
            status_ = s;
            seek_read_.status = s;
            seek_read_.stage = LookUpStage::Error;
            return false;
        }
        delete block_reader_;
        block_reader_ = reader;
        return true;
    }

    // Drops any prefetched index block.  An in-flight read is only marked
    // cancelled: its destination buffer must stay untouched until it completes.
    void clear_readahead() {
        delete readahead_reader_;
        readahead_reader_ = nullptr;
        readahead_block_ = 0;
        if (readahead_in_flight_) {
            readahead_cancelled_ = true;
        } else {
            readahead_state_.stage = LookUpStage::Idle;
            readahead_done_ = true;
        }
    }

    // Decodes the completed readahead into a resident reader, or drops it.
    void install_readahead_() {
        readahead_done_ = true;
        if (readahead_cancelled_) {
            readahead_cancelled_ = false;
            readahead_state_.stage = LookUpStage::Idle;
            return;
        }
        IndexBlockReader *reader = nullptr;
        Status s = decode_index_state_(readahead_state_, &reader);
        if (!s.is_ok()) {
            readahead_state_.stage = LookUpStage::Idle;
            return;
        }
        delete readahead_reader_;
        readahead_reader_ = reader;
    }

    void invalidate() {
        valid_ = false;
        clear_seek_prefetch();
        clear_readahead();
        delete block_reader_;
        block_reader_ = nullptr;
    }

    void load_block(size_t block_index) {
        block_index_ = block_index;
        clear_seek_prefetch();

        // Take the prefetched block when it is the one we need; swapping the
        // buffers keeps the reader's views pointing at live storage.
        if (readahead_reader_ != nullptr && readahead_block_ == block_index) {
            std::swap(stored_buf_, readahead_state_.index);
            std::swap(plain_buf_, readahead_state_.plain_index);
            delete block_reader_;
            block_reader_ = readahead_reader_;
            readahead_reader_ = nullptr;
            readahead_block_ = 0;
            readahead_state_.stage = LookUpStage::Idle;
            readahead_done_ = true;
            return;
        }

        // A prefetch for another block is stale.
        delete readahead_reader_;
        readahead_reader_ = nullptr;
        readahead_block_ = 0;
        if (readahead_in_flight_) {
            // The synchronous read below wins; let the in-flight result be
            // discarded when the batch completes.
            readahead_cancelled_ = true;
        }

        delete block_reader_;
        block_reader_ = nullptr;

        // The summary gives this block's absolute offset and on-disk length.
        const SummaryEntry &entry = owner_->summary_->entry(block_index);
        std::string_view block;
        Status s =
            owner_->read_index_block_(entry.block_offset, entry.compressed_size, &stored_buf_, &plain_buf_, &block);
        if (!s.is_ok()) {
            status_ = s;
            invalidate();
            return;
        }
        block_reader_ = new IndexBlockReader(block, owner_->block_size_);
        if (!block_reader_->valid()) {
            status_ = Status::corruption("invalid index block");
            invalidate();
        }
    }

    // Positions on the next in-range slot, crossing block boundaries.
    void settle_forward() {
        while (true) {
            if (block_reader_ == nullptr) {
                invalidate();
                return;
            }
            if (slot_ < block_reader_->size()) {
                valid_ = true;
                return;
            }
            if (block_index_ + 1 >= block_count_) {
                invalidate();
                return;
            }
            load_block(block_index_ + 1);
            if (!status_.is_ok()) {
                invalidate();
                return;
            }
            slot_ = 0;
        }
    }

    // Positions on the previous in-range slot, crossing block boundaries.
    void settle_backward() {
        while (true) {
            if (block_reader_ == nullptr) {
                invalidate();
                return;
            }
            if (slot_ >= block_reader_->size()) {
                // Positioned just past the end of a block: step to its last
                // entry, or move to the previous block if it is empty.
                if (block_reader_->size() == 0) {
                    if (block_index_ == 0) {
                        invalidate();
                        return;
                    }
                    load_block(block_index_ - 1);
                    if (!status_.is_ok()) {
                        invalidate();
                        return;
                    }
                    slot_ = block_reader_->size();
                    continue;
                }
                slot_ = block_reader_->size() - 1;
                continue;
            }
            valid_ = true;
            return;
        }
    }

    TableReader *owner_;
    Comparator comparator_;
    size_t block_count_;

    size_t block_index_ = 0;
    IndexBlockReader *block_reader_ = nullptr;
    // On-disk (compressed) bytes of the resident block and, when decompressed,
    // the plain bytes the reader parses.
    std::string stored_buf_;
    std::string plain_buf_;

    size_t slot_ = 0;
    bool valid_ = false;

    // Keeps the resolved data block alive across value_ref() calls; the view
    // returned by value_ref() points into it.
    mutable Cache::Handle resident_data_block_;

    // Staged-seek state.  seek_read_.stage drives the three seek stages, and
    // finish_seek() moves its block pin into resident_data_block_ so value_ref()
    // can skip a cache lookup.
    TableReadState seek_read_;
    std::string seek_target_;
    bool seek_data_pinned_ = false;
    size_t seek_pinned_block_ = 0;
    size_t seek_pinned_slot_ = 0;

    // Read-ahead of the next index block: a second TableReadState holds its
    // queued read, and readahead_reader_ borrows that state's buffers once
    // decoded.  `cancelled` lets a reposition discard a result that is still
    // in flight without touching its destination buffer.
    TableReadState readahead_state_;
    IndexBlockReader *readahead_reader_ = nullptr;
    size_t readahead_block_ = 0;
    bool readahead_in_flight_ = false;
    bool readahead_done_ = true;
    bool readahead_cancelled_ = false;

    mutable std::string scratch_;
    mutable Status status_;
};

EntryIterator *TableReader::new_iterator() {
    if (summary_ == nullptr || !summary_->valid()) {
        return new_error_entry_iterator(Status::corruption("invalid table"));
    }
    if (summary_->empty()) { return new_empty_entry_iterator(); }
    return new TableIterator(this);
}

Status TableReader::read_index_block_(uint64_t block_offset, uint32_t compressed_size, std::string *stored_buf,
                                      std::string *plain_buf, std::string_view *out) const {
    *out = std::string_view();
    if (block_offset > index_file_size_ || compressed_size > index_file_size_ - block_offset) {
        return Status::corruption("table reader: index block out of range", std::to_string(block_offset));
    }

    std::string_view stored;
    Status s = read_exact(index_reader_, block_offset, compressed_size, stored_buf, &stored);
    if (!s.is_ok()) { return s; }

    return decode_index_block_(std::string_view(stored_buf->data(), compressed_size), plain_buf, out);
}

Status TableReader::decode_index_block_(std::string_view stored, std::string *plain_buf, std::string_view *out) const {
    // A zero compressor id means the block is stored verbatim regardless of
    // the function pointers the caller supplied.
    if (compressor_.id == 0 || compressor_.uncompress == nullptr) {
        if (stored.size() != block_size_) {
            return Status::corruption("table reader: index block not block sized", std::to_string(stored.size()));
        }
        *out = stored;
        return Status::ok();
    }
    if (compressor_.get_uncompressed_length == nullptr) {
        return Status::corruption("table reader: compressor lacks length reporting");
    }

    size_t uncompressed_size = 0;
    if (!compressor_.get_uncompressed_length(stored.data(), stored.size(), &uncompressed_size)) {
        return Status::corruption("table reader: bad compressed index block length");
    }
    if (uncompressed_size != block_size_) {
        return Status::corruption("table reader: index block not block sized after decompression",
                                  std::to_string(uncompressed_size));
    }
    plain_buf->resize(uncompressed_size);
    if (!compressor_.uncompress(stored.data(), stored.size(), plain_buf->data())) {
        return Status::corruption("table reader: index block decompression failed");
    }
    *out = std::string_view(plain_buf->data(), uncompressed_size);
    return Status::ok();
}

Status TableReader::resolve_inline_(const ValueHandle &handle, std::string_view expected_key, Cache::Handle *pin,
                                    const DataBlockEntry **entry_out) const {
    if (pin == nullptr || entry_out == nullptr) {
        return Status::invalid_argument("table reader: null inline resolution output");
    }
    pin->reset();
    *entry_out = nullptr;
    if (is_vlog_blob_type(handle.type)) {
        return Status::invalid_argument("table reader: blob handle passed to inline resolver");
    }
    // The inline slot addresses a framed data block: file_id is the block's
    // absolute offset and data_offset is the record's offset inside the
    // uncompressed block.
    if (handle.size < kDataBlockHeaderSize) {
        return Status::corruption("table reader: data block too small", std::to_string(handle.size));
    }

    CachedDataBlock *block = nullptr;
    Status s =
        block_cache_.get(data_reader_, data_file_size_, data_compressor_, handle.file_id, handle.size, pin, &block);
    if (!s.is_ok()) { return s; }
    if (!*pin || block == nullptr) { return Status::corruption("table reader: block cache returned no block"); }

    // *pin owns block for the rest of this call, so no error path frees it.
    size_t index = 0;
    if (!block->reader->find_offset(handle.data_offset, &index)) {
        return Status::corruption("table reader: data block record offset missing", std::to_string(handle.data_offset));
    }
    const DataBlockEntry &record = block->reader->entry(index);
    if (record.key != expected_key) { return Status::corruption("table reader: data record key mismatch"); }
    if (record.type != handle.type) { return Status::corruption("table reader: index/data record metadata mismatch"); }

    *entry_out = &record;
    return Status::ok();
}

Status TableReader::read_inline_value_(const ValueHandle &handle, std::string_view expected_key, std::string *scratch,
                                       std::string_view *value) const {
    if (value == nullptr) { return Status::invalid_argument("table reader: null inline value output"); }
    *value = std::string_view();

    Cache::Handle pin;
    const DataBlockEntry *record = nullptr;
    Status s = resolve_inline_(handle, expected_key, &pin, &record);
    if (!s.is_ok()) { return s; }

    // The record view belongs to the pinned block; copy it so the caller's value
    // survives once the pin is released.
    scratch->assign(record->value.data(), record->value.size());
    *value = std::string_view(scratch->data(), scratch->size());
    return Status::ok();
}

Status TableReader::make_value_ref_(const IndexBlockEntry &entry, std::string *scratch, ValueRef *out) const {
    // Reset first so a reused output cannot retain a stale inline view (or a
    // blob handle) from a previous call.
    *out = ValueRef();
    out->type = entry.handle.type;
    out->timestamp = entry.timestamp;
    if (is_vlog_blob_type(entry.handle.type)) {
        out->blob.file_id = entry.handle.file_id;
        out->blob.size = entry.handle.size;
        out->blob.offset = entry.handle.data_offset;
        return Status::ok();
    }

    std::string_view inline_value;
    Status s = read_inline_value_(entry.handle, entry.key, scratch, &inline_value);
    if (!s.is_ok()) { return s; }
    out->inline_value = inline_value;
    return Status::ok();
}

ReadBatcher *TableReader::batcher() const { return index_reader_ != nullptr ? index_reader_->batcher() : nullptr; }

// ---------------------------------------------------------------------------
// Staged point lookup
// ---------------------------------------------------------------------------
//
// The three stages below are the same read path get_raw() walks, split at the
// two points where a round trip happens so a caller can batch across tables:
// summary -> index block (stage 1), binary search -> data block (stage 2), and
// record -> ValueRef (stage 3).  Nothing about parsing or encoding changes; only
// who performs the read and when.

bool TableReader::filter_admits_(TableReadState &state, std::string_view key) const {
    // A definite filter miss means the key is absent, so skip all I/O.
    if (filter_ != nullptr && !filter_->key_may_match(key)) {
        state.status = Status::not_found("key not in table filter", std::string(key));
        state.stage = LookUpStage::NotFound;
        return false;
    }
    return true;
}

void TableReader::queue_index_read_(const SummaryEntry &entry, TableReadState *state, ReadBatcher *batcher,
                                    bool *done) const {
    std::string &buf = state->index;
    buf.resize(entry.compressed_size);

    // Drop state.plain's capacity: the previous lookup may have kept a large
    // block alive well past the value that needed it.
    std::string().swap(state->plain);
    state->block.reset();
    state->out = ValueRef();

    // Mirror read_exact()'s contract: on success *status is ok and *bytes_read
    // is the number transferred, which a batched read may report short.
    state->status = Status::ok();
    if (batcher != nullptr && index_reader_
                                  ->queue_read(entry.block_offset, entry.compressed_size, buf.data(), &state->status,
                                               &state->bytes_read, done)
                                  .is_ok()) {
        state->stage = LookUpStage::IndexQueued;
        return;
    }

    // No batcher, or a reader that cannot batch: read it now.  A value past the
    // block length is a short read, not an error.
    std::string_view view;
    state->status = index_reader_->read(entry.block_offset, entry.compressed_size, &view, buf.data());
    if (state->status.is_ok() && view.size() != entry.compressed_size) {
        state->status = Status::corruption("table reader: short index block read", std::to_string(view.size()));
    }
    state->bytes_read = view.size();
    state->stage = state->status.is_ok() ? LookUpStage::IndexReady : LookUpStage::Error;
}

void TableReader::prepare_index_(TableReadState &state, std::string_view key, ReadBatcher *batcher) const {
    const size_t block_index = summary_->lower_bound(key);
    if (block_index >= summary_->size()) {
        state.status = Status::not_found("key not in table range", std::string(key));
        state.stage = LookUpStage::NotFound;
        return;
    }

    queue_index_read_(summary_->entry(block_index), &state, batcher);
}

void TableReader::prepare_index_read(TableReadState &state, std::string_view key, ReadBatcher *batcher) const {
    state.stage = LookUpStage::Idle;
    state.block.reset();
    state.out = ValueRef();
    state.slot = 0;
    state.status = Status::ok();
    if (!filter_admits_(state, key)) { return; }
    prepare_index_(state, key, batcher);
}

bool TableReader::decode_index_(TableReadState *state, std::string_view key) const {
    if (state->stage == LookUpStage::IndexQueued) {
        // The batch has run: a per-op error outranks a short read.
        if (!state->status.is_ok()) {
            state->stage = LookUpStage::Error;
            return false;
        }
    } else if (state->stage != LookUpStage::IndexReady) {
        state->status = Status::corruption("table reader: prepare_data_read without an index block");
        state->stage = LookUpStage::Error;
        return false;
    }

    std::string_view block;
    // The batched path may leave the buffer longer than the block, so the
    // extent is the byte count the read transferred; the batch has already run
    // by now, so state->bytes_read still describes this lookup's read.
    Status s =
        decode_index_block_(std::string_view(state->index).substr(0, state->bytes_read), &state->plain_index, &block);
    if (!s.is_ok()) {
        state->status = s;
        state->stage = LookUpStage::Error;
        return false;
    }

    IndexBlockReader reader(block, block_size_);
    if (!reader.valid()) {
        state->status = Status::corruption("table reader: invalid index block");
        state->stage = LookUpStage::Error;
        return false;
    }

    const size_t slot = reader.lower_bound(key, comparator_);
    if (slot >= reader.size() || comparator_.compare(reader.key(slot), key) != 0) {
        state->status = Status::not_found("key not found", std::string(key));
        state->stage = LookUpStage::NotFound;
        return false;
    }

    state->slot = slot;
    return true;
}

bool TableReader::select_and_prepare_data_(TableReadState *state, ReadBatcher *batcher) const {
    // Re-parse the resident block rather than keep the reader: `reader` borrows
    // state->index, which only needs to outlive this call.
    IndexBlockReader reader(state->index, block_size_);
    const IndexBlockEntry entry = reader.entry(state->slot);

    // A blob slot addresses the vLog, which this reader never touches, so the
    // lookup is complete once the handle is copied into state.out.
    if (is_vlog_blob_type(entry.handle.type)) {
        // Keep the resident block rather than drop it: its views stay valid while
        // the block stays in the cache, so state.out may still point into it.
        state->block.reset();
        state->status = make_value_ref_(entry, nullptr, &state->out);
        state->stage = state->status.is_ok() ? LookUpStage::Finished : LookUpStage::Error;
        return false;
    }

    const ValueHandle &handle = entry.handle;
    if (handle.size < kDataBlockHeaderSize) {
        state->status = Status::corruption("table reader: data block too small", std::to_string(handle.size));
        state->stage = LookUpStage::Error;
        return false;
    }
    if (handle.file_id > data_file_size_ || handle.size > data_file_size_ - handle.file_id) {
        state->status = Status::corruption("table reader: data block out of range", std::to_string(handle.file_id));
        state->stage = LookUpStage::Error;
        return false;
    }

    // A block already in the cache needs no read; finish_get() resolves it from
    // the pin.  Dropping the previous pin first means a lookup that lands on the
    // block it just finished reuses this lookup's slot instead of its predecessor's.
    state->block.reset();
    if (lookup_resident_block_(handle, state)) {
        state->stage = LookUpStage::DataReady;
        return false;
    }

    queue_data_read_(handle, state, batcher);
    return state->stage == LookUpStage::DataQueued || state->stage == LookUpStage::DataReady;
}

void TableReader::queue_data_read_(const ValueHandle &handle, TableReadState *state, ReadBatcher *batcher) const {
    std::string &buf = state->data;
    buf.resize(handle.size);
    if (buf.capacity() > 4 * handle.size) { std::string().swap(buf); }

    state->status = Status::ok();
    if (batcher != nullptr &&
        data_reader_->queue_read(handle.file_id, handle.size, buf.data(), &state->status, &state->bytes_read).is_ok()) {
        state->stage = LookUpStage::DataQueued;
        return;
    }

    // No batcher, or a reader that cannot batch: read it now and publish it the
    // same way the batched path does once its batch completes, so both leave a
    // lookup that finish_get() can resolve without reading anything.
    std::string_view view;
    state->status = data_reader_->read(handle.file_id, handle.size, &view, buf.data());
    if (state->status.is_ok() && view.size() != handle.size) {
        state->status = Status::corruption("table reader: short data block read", std::to_string(view.size()));
    }
    state->bytes_read = view.size();
    if (!state->status.is_ok()) {
        state->stage = LookUpStage::Error;
        return;
    }

    state->status = adopt_data_block_(handle, state);
    state->stage = state->status.is_ok() ? LookUpStage::DataReady : LookUpStage::Error;
}

bool TableReader::prepare_data_read(TableReadState &state, std::string_view key, ReadBatcher *batcher) const {
    if (state.stage == LookUpStage::Idle || state.stage == LookUpStage::Finished) {
        // The caller skipped stage 1 (or is reusing a finished lookup): run it.
        prepare_index_read(state, key, batcher);
        if (state.stage == LookUpStage::IndexQueued) {
            // Nothing has read the block yet, so this stage cannot go further
            // without the caller submitting and calling back in.
            return false;
        }
        if (state.stage != LookUpStage::IndexReady) { return false; }
    }
    if (!decode_index_(&state, key)) { return false; }
    return select_and_prepare_data_(&state, batcher);
}

std::string TableReader::block_cache_key_(const ValueHandle &handle) const {
    // The key is derived from the block's location in -Data.adb, which is what
    // BlockCache::get() keys on, so a block read here and one read by the
    // iterator or get_raw() share a slot.  Returned by value: the key must
    // outlive the call that looks it up.
    char key[BlockCache::kKeySize];
    block_cache_.make_key(handle.file_id, handle.size, key);
    return std::string(key, sizeof(key));
}

bool TableReader::lookup_resident_block_(const ValueHandle &handle, TableReadState *state) const {
    state->block = block_cache_.lookup(handle.file_id, handle.size);
    return static_cast<bool>(state->block);
}

Status TableReader::adopt_data_block_(const ValueHandle &handle, TableReadState *state) const {
    Status s = decode_data_block_(handle, state);
    if (!s.is_ok()) { return s; }

    // decode_data_block_ left the block in state.plain, so hand it straight to
    // the cache: the cache-owned block then owns those bytes and state.data is
    // free to be reused as the next lookup's transport buffer.
    CachedDataBlock *block = BlockCache::make_block(std::move(state->plain), &s);
    state->plain.clear();
    if (block == nullptr) { return s; }

    char key[BlockCache::kKeySize];
    state->block = block_cache_.adopt(block_cache_key_(handle), block);
    return Status::ok();
}

Status TableReader::decode_data_block_(const ValueHandle &handle, TableReadState *state) const {
    // The batched path quantifies the block by the byte count the read
    // reported, not by the buffer size (a batched read may leave it longer).
    const std::string_view block(state->data.data(), std::min(state->data.size(), static_cast<size_t>(handle.size)));
    std::string &plain = state->plain;
    if (block.size() != handle.size) {
        return Status::corruption("table reader: short data block read", std::to_string(block.size()));
    }
    if (block.size() < kDataBlockHeaderSize) {
        return Status::corruption("table reader: data block too small", std::to_string(block.size()));
    }

    uint32_t uncompressed_size = 0;
    std::string_view payload;
    if (!parse_data_block_header(block, &uncompressed_size, &payload)) {
        return Status::corruption("table reader: missing data block header");
    }
    if (uncompressed_size == 0 || uncompressed_size > kMaxDataBlockSize) {
        return Status::corruption("table reader: invalid data block length", std::to_string(uncompressed_size));
    }

    // A zero codec id means the block was stored verbatim after its header.
    if (data_compressor_.id == 0 || data_compressor_.uncompress == nullptr) {
        if (payload.size() != uncompressed_size) {
            return Status::corruption("table reader: data block length mismatch", std::to_string(payload.size()));
        }
        plain.assign(payload.data(), payload.size());
        return Status::ok();
    }
    if (data_compressor_.get_uncompressed_length == nullptr) {
        return Status::corruption("table reader: data codec lacks length reporting");
    }

    size_t reported_size = 0;
    if (!data_compressor_.get_uncompressed_length(payload.data(), payload.size(), &reported_size)) {
        return Status::corruption("table reader: bad compressed data block");
    }
    if (reported_size != uncompressed_size) {
        return Status::corruption("table reader: data block length mismatch", std::to_string(reported_size));
    }

    plain.resize(uncompressed_size);
    if (!data_compressor_.uncompress(payload.data(), payload.size(), plain.data())) {
        return Status::corruption("table reader: data block decompression failed");
    }
    return Status::ok();
}

Status TableReader::find_record_(const ValueHandle &handle, std::string_view block, std::string_view expected_key,
                                 DataBlockEntry *record_out) const {
    *record_out = DataBlockEntry();
    DataBlockReader reader(block);
    if (!reader.valid()) { return Status::corruption("table reader: invalid data block"); }

    size_t index = 0;
    if (!reader.find_offset(handle.data_offset, &index)) {
        return Status::corruption("table reader: data block record offset missing", std::to_string(handle.data_offset));
    }
    const DataBlockEntry &record = reader.entry(index);
    if (record.key != expected_key) { return Status::corruption("table reader: data record key mismatch"); }
    if (record.type != handle.type) { return Status::corruption("table reader: index/data record metadata mismatch"); }

    // The caller gets the entry by value: its views point into `block`, but the
    // reader that parsed it dies here.
    *record_out = record;
    return Status::ok();
}

Status TableReader::read_inline_value_from_state_(const IndexBlockEntry &entry, TableReadState *state) const {
    // When a block is pinned, parse the cache-owned bytes rather than a copy in
    // state.plain: that is what lets the returned view stay valid while the pin
    // is held.  Without a pin (a reader that bypassed the cache),
    // decode_data_block_ left the block in state.plain.
    std::string_view block = state->plain;
    if (state->block) {
        const auto *resident = static_cast<CachedDataBlock *>(state->block.value());
        if (resident == nullptr) { return Status::corruption("table reader: pinned block is missing"); }
        block = resident->bytes;
    }

    DataBlockEntry record;
    Status s = find_record_(entry.handle, block, entry.key, &record);
    if (!s.is_ok()) { return s; }

    if (state->block) {
        state->out.inline_value = record.value;
        return Status::ok();
    }

    // Nothing pins the block, so the caller must not be left holding a view
    // into this state.
    state->value.assign(record.value.data(), record.value.size());
    state->out.inline_value = std::string_view(state->value.data(), state->value.size());
    return Status::ok();
}

bool TableReader::resolve_and_finish_(TableReadState *state) const {
    IndexBlockReader reader(state->index, block_size_);
    if (state->slot >= reader.size()) {
        state->status = Status::corruption("table reader: index slot out of range", std::to_string(state->slot));
        state->stage = LookUpStage::Error;
        return false;
    }

    const IndexBlockEntry entry = reader.entry(state->slot);

    if (state->stage == LookUpStage::DataQueued) {
        // The batch has run: a per-op error outranks a short read.  The byte
        // count is only meaningful here, so the error paths below may also use
        // it freely.
        if (!state->status.is_ok() || state->bytes_read != state->data.size()) {
            state->block.reset();
            if (state->status.is_ok()) {
                state->status =
                    Status::corruption("table reader: short data block read", std::to_string(state->bytes_read));
            }
            state->stage = LookUpStage::Error;
            return false;
        }

        // The read buffer is transport only: decode and publish it, so the next
        // lookup for this block needs no read at all and
        // read_inline_value_from_state_ finds it in state.plain.  The failure is
        // kept here: status itself was just overwritten above.
        Status decode = adopt_data_block_(entry.handle, state);
        if (!decode.is_ok()) {
            state->block.reset();
            state->status = decode;
            state->stage = LookUpStage::Error;
            return false;
        }
    } else if (state->stage != LookUpStage::DataReady) {
        state->status = Status::corruption("table reader: finish_get without a resolved data block");
        state->stage = LookUpStage::Error;
        return false;
    }

    state->status = read_inline_value_from_state_(entry, state);
    if (!state->status.is_ok()) {
        state->block.reset();
        state->stage = LookUpStage::Error;
        return false;
    }
    state->stage = LookUpStage::Finished;
    return true;
}

bool TableReader::finish_get(TableReadState &state) const {
    // A blob or a slot with no data record is already complete: out holds its
    // handle.  A miss or a failure has nothing to finish.
    if (state.stage == LookUpStage::Finished) { return state.status.is_ok(); }
    if (state.stage != LookUpStage::DataQueued && state.stage != LookUpStage::DataReady) { return false; }

    state.out = ValueRef();
    return resolve_and_finish_(&state);
}

void TableReader::call(TableReadState &state, std::string_view key) const {
    prepare_index_read(state, key, nullptr);
    // The synchronous prepare stages land on IndexReady/DataReady, so nothing
    // is ever left queued here; both calls are just the next stage of the same
    // lookup.
    prepare_data_read(state, key, nullptr);
    if (!finish_get(state) && state.stage != LookUpStage::NotFound && state.stage != LookUpStage::Error) {
        state.status = Status::corruption("table reader: lookup did not complete");
        state.stage = LookUpStage::Error;
    }
}

} // namespace ANCHORDB_NAMESPACE
