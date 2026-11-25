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

#include "file_meta.h"
#include "port/port.h" // IWYU pragma: keep
#include "read_batcher.h"
#include "sync_io.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace ANCHORDB_NAMESPACE {

namespace {

constexpr uintptr_t kAlignMask = static_cast<uintptr_t>(kDirectIOAlignment) - 1;

uintptr_t align_down(uintptr_t x) { return x & ~kAlignMask; }
uintptr_t align_up(uintptr_t x) { return (x + kAlignMask) & ~kAlignMask; }

// Upper bound on completions reaped with one io_uring_peek_batch_cqe() call.
constexpr unsigned kMaxReapBatch = 256;

// Context label for errors that cannot name a single file, since one batch may
// span several descriptors.
constexpr const char *kBatchContext = "<batch>";

} // namespace

bool PosixReadBatcher::add(int fd, const BatchReadOp &op) {
    // One generation at a time: a completed generation may be cleared to make
    // room, but never while operations are still in flight (their destination
    // buffers must stay untouched).  Callers poll()/wait() before re-arming.
    if (submitted_) {
        if (remaining_ != 0) { return false; }
        finish();
    }
    ops_.push_back(PendingOp{fd, op});
    return true;
}

Status PosixReadBatcher::submit() {
    if (submitted_) { return first_error_; }
    if (ops_.empty()) { return Status::ok(); }

    plan();
    reset_results();
    submitted_ = true;

#if HAS_IO_URING
    if (ring_ != nullptr) {
        Status s = queue_submissions();
        if (s.is_ok()) {
            int ret = io_uring_submit(ring_);
            if (ret < 0) { s = posix_error(kBatchContext, -ret); }
        }
        if (!s.is_ok()) {
            // Reap anything already in flight before failing the rest, so the
            // caller can safely release its buffers when this returns.
            while (remaining_ > 0) {
                if (reap_available()) { continue; }
                if (io_uring_submit_and_wait(ring_, static_cast<unsigned>(remaining_)) < 0) { break; }
            }
            fail_unfinished(s);
        }
        return first_error_;
    }
#endif

    // No asynchronous backend: submit performs the reads here.
    execute_sync();
    return first_error_;
}

bool PosixReadBatcher::poll() {
    if (ops_.empty()) { return true; }
    if (!submitted_) { submit(); }
#if HAS_IO_URING
    if (ring_ != nullptr && remaining_ > 0) { reap_available(); }
#endif
    return remaining_ == 0;
}

Status PosixReadBatcher::wait() {
    if (ops_.empty()) {
        submitted_ = false;
        return Status::ok();
    }
    if (!submitted_) { submit(); }
#if HAS_IO_URING
    if (ring_ != nullptr) {
        while (remaining_ > 0) {
            if (reap_available()) { continue; }
            int ret = io_uring_submit_and_wait(ring_, static_cast<unsigned>(remaining_));
            if (ret < 0) {
                fail_unfinished(posix_error(kBatchContext, -ret));
                break;
            }
        }
    }
#endif
    return finish();
}

Status PosixReadBatcher::execute() {
    submit();
    return wait();
}

void PosixReadBatcher::plan() {
    const size_t count = ops_.size();

    // Under direct I/O the kernel demands block-aligned buffers, offsets and
    // lengths, so re-align the request and, when that changes the destination
    // or extent, read through an aligned bounce buffer instead.
    issued_.clear();
    issued_.reserve(count);
    for (const PendingOp &pending : ops_) {
        const BatchReadOp &op = pending.op;
        IssuedOp issued{op.offset, op.size, false};
        if (direct_io_ && op.size != 0) {
            issued.offset = align_down(op.offset);
            issued.size = static_cast<size_t>(align_up(op.offset + op.size) - issued.offset);
            issued.bounced = op.dst == nullptr || (reinterpret_cast<uintptr_t>(op.dst) & kAlignMask) != 0 ||
                             issued.offset != op.offset || issued.size != op.size;
        }
        issued_.push_back(issued);
    }

    // Give every bounced read its own block-aligned scratch, reusing prior
    // capacity.  AlignedBuffer buffers are never shrunk.
    for (size_t i = 0; i < count; i++) {
        if (!issued_[i].bounced) { continue; }
        if (bounce_.size() <= i) { bounce_.resize(i + 1); }
        if (bounce_[i].capacity() < issued_[i].size) { bounce_[i].re_allocate(issued_[i].size); }
    }
}

void PosixReadBatcher::reset_results() {
    completed_.assign(ops_.size(), 0);
    remaining_ = ops_.size();
    first_error_ = Status::ok();
    for (const PendingOp &pending : ops_) {
        if (pending.op.status != nullptr) { *pending.op.status = Status::ok(); }
        if (pending.op.bytes_read != nullptr) { *pending.op.bytes_read = 0; }
        if (pending.op.done != nullptr) { *pending.op.done = false; }
    }
}

char *PosixReadBatcher::io_dst(size_t i) const { return issued_[i].bounced ? bounce_[i].data() : ops_[i].op.dst; }

void PosixReadBatcher::publish(size_t i, size_t got) {
    const BatchReadOp &op = ops_[i].op;
    if (issued_[i].bounced) {
        // A bounce buffer holds whole blocks spanning the request; copy out just
        // the window the caller asked for.
        const size_t skip = op.offset - issued_[i].offset;
        const size_t payload = got > skip ? std::min(got - skip, op.size) : 0;
        if (payload > 0 && op.dst != nullptr) { std::memcpy(op.dst, bounce_[i].data() + skip, payload); }
        if (op.bytes_read != nullptr) { *op.bytes_read = payload; }
    } else if (op.bytes_read != nullptr) {
        *op.bytes_read = got;
    }

    if (op.done != nullptr) { *op.done = true; }
    if (i < completed_.size() && completed_[i] == 0) {
        completed_[i] = 1;
        if (remaining_ > 0) { --remaining_; }
    }
}

void PosixReadBatcher::record_error(size_t i, const Status &s) {
    if (ops_[i].op.status != nullptr) { *ops_[i].op.status = s; }
    if (ops_[i].op.done != nullptr) { *ops_[i].op.done = true; }
    if (i < completed_.size() && completed_[i] == 0) {
        completed_[i] = 1;
        if (remaining_ > 0) { --remaining_; }
    }
    if (first_error_.is_ok()) { first_error_ = s; }
}

void PosixReadBatcher::fail_unfinished(const Status &s) {
    for (size_t i = 0; i < ops_.size(); i++) {
        if (i < completed_.size() && completed_[i] != 0) { continue; }
        record_error(i, s);
    }
    remaining_ = 0;
    if (first_error_.is_ok()) { first_error_ = s; }
}

Status PosixReadBatcher::finish() {
    const Status result = first_error_;
    ops_.clear();
    completed_.clear();
    remaining_ = 0;
    submitted_ = false;
    first_error_ = Status::ok();
    return result;
}

void PosixReadBatcher::execute_sync() {
    for (size_t i = 0; i < ops_.size(); i++) {
        char *dst = io_dst(i);
        if (dst == nullptr) {
            record_error(i, Status::io_error("read batcher: cannot allocate aligned buffer", kBatchContext));
            continue;
        }
        size_t got = 0;
        Status s = file_internal::sync_pread(kBatchContext, ops_[i].fd, dst, issued_[i].size, issued_[i].offset, &got);
        if (!s.is_ok()) {
            record_error(i, s);
            continue;
        }
        publish(i, got);
    }
    remaining_ = 0;
}

#if HAS_IO_URING

Status PosixReadBatcher::queue_submissions() {
    const size_t total = ops_.size();
    size_t prepared = 0;

    // Fill the SQ until the batch is queued or the ring is full.  A full ring
    // is not an error: flush what is queued so the slots come back for reuse.
    // Submitting early only overlaps more I/O and never truncates the batch.
    while (prepared < total) {
        if (issued_[prepared].bounced && !bounce_[prepared].is_allocated()) {
            record_error(prepared, Status::io_error("read batcher: cannot allocate aligned buffer", kBatchContext));
            ++prepared;
            continue;
        }

        struct io_uring_sqe *sqe = io_uring_get_sqe(ring_);
        if (sqe == nullptr) {
            int ret = io_uring_submit(ring_);
            if (ret < 0) { return posix_error(kBatchContext, -ret); }
            continue;
        }

        io_uring_prep_read(sqe, ops_[prepared].fd, io_dst(prepared), issued_[prepared].size, issued_[prepared].offset);
        // The user_data word only maps a completion back to its op; the low 32
        // bits are opaque and never dereferenced.
        io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(prepared)));
        ++prepared;
    }
    return Status::ok();
}

bool PosixReadBatcher::reap_available() {
    struct io_uring_cqe *cqes[kMaxReapBatch];
    const unsigned got = io_uring_peek_batch_cqe(ring_, cqes, kMaxReapBatch);

    for (unsigned k = 0; k < got; k++) {
        struct io_uring_cqe *cqe = cqes[k];
        const size_t index = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
        const int res = cqe->res;
        io_uring_cqe_seen(ring_, cqe);

        // Skip completions that are not part of this generation (or already
        // counted), so a stray CQE cannot corrupt the progress counter.
        if (index >= ops_.size() || completed_[index] != 0) { continue; }
        if (res < 0) {
            record_error(index, posix_error(kBatchContext, -res));
        } else {
            publish(index, static_cast<size_t>(res));
        }
    }
    return got > 0;
}

#endif // HAS_IO_URING

} // namespace ANCHORDB_NAMESPACE
