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
#include "anchordb/env.h"
#include "anchordb/status.h"
#include "port/port.h" // IWYU pragma: keep
#include "util/aligned_buffer.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ANCHORDB_NAMESPACE {

// ReadBatcher backed by an io_uring ring, falling back to pread(2) when no ring
// is supplied or when the library was built without io_uring.
//
// The ring is borrowed: the caller creates it (PosixEnv::get_thread_local_ring()
// is the usual source), keeps it alive for this batcher's lifetime, and must
// not share it with another user while a batch is in flight, because poll() and
// wait() reap the ring's outstanding completions.
//
// One generation is in flight at a time.  submit() starts it without waiting;
// poll() makes progress without blocking; wait()/execute() finish it and clear
// the queue.  Adding another operation is rejected while a generation is still
// in flight, so a caller that wants to re-arm must wait() first.
//
// With `direct_io` set, each operation is re-aligned to the block size and read
// through bounce buffers, so the caller may use an unaligned `dst`, `offset` or
// `size` even on an O_DIRECT descriptor.
class PosixReadBatcher final : public ReadBatcher {
  public:
    // A batcher needs a target ring, so there is no default constructor.
    PosixReadBatcher() = delete;

    explicit PosixReadBatcher(struct io_uring *ring, bool direct_io = true) : ring_(ring), direct_io_(direct_io) {}

    PosixReadBatcher(const PosixReadBatcher &) = delete;
    PosixReadBatcher &operator=(const PosixReadBatcher &) = delete;

    // Queues `op` for the next submit(), reading it from `fd`.  Performs no I/O.
    // Returns false when a previous generation is still in flight.
    bool add(int fd, const BatchReadOp &op) override;

    // Starts every queued read without waiting for completions.
    Status submit() override;

    // Reaps completions that are already available; true when all are done.
    bool poll() override;

    // Blocks until every queued read completes, then clears the queue.
    Status wait() override;

    // submit() + wait().
    Status execute() override;

  private:
    // One staged read: the caller's operation plus the descriptor add() was told
    // to read it from.  The fd lives here rather than in BatchReadOp so the
    // public operation describes only what to read, not which file owns it.
    struct PendingOp {
        int fd;
        BatchReadOp op;
    };

    // Per-operation bookkeeping for one generation.  Entry `i` records how
    // operation `i` was actually issued.
    struct IssuedOp {
        uint64_t offset; // aligned read offset, block-aligned under direct I/O
        size_t size;     // aligned read size, block-aligned under direct I/O
        bool bounced;    // read goes through bounce_, not straight into dst
    };

    // Plans how each stage op is issued and sizes the bounce buffers the batch
    // will need, so the I/O loops below stay about I/O.
    void plan();

    // Resets every stage op's outcome/result storage for a new generation.
    void reset_results();

    // Publishes the outcome of operation `i`: `got` is what the kernel
    // transferred into its issued buffer.
    void publish(size_t i, size_t got);
    void record_error(size_t i, const Status &s);

    char *io_dst(size_t i) const;

    // Runs the stage one pread(2) at a time, completing the whole generation.
    // Used with no ring and on builds without io_uring.
    void execute_sync();

    // Marks every not-yet-completed operation as failed with `s`.
    void fail_unfinished(const Status &s);

    // Clears the generation and returns its first error.
    Status finish();

#if HAS_IO_URING
    // Queues an SQE for every op, submitting early when the ring fills.
    Status queue_submissions();
    // Reaps available CQEs without blocking.  Returns true when at least one
    // completion was processed.
    bool reap_available();
#endif

    struct io_uring *const ring_;
    const bool direct_io_;

    // Staged operations in add() order; drained by wait()/execute().
    std::vector<PendingOp> ops_;

    // Scratch reused across generations so a steady workload stops
    // allocating.  Each vector keeps its capacity; bounce buffers only grow.
    std::vector<IssuedOp> issued_;
    // Block-aligned bounce buffers for reads that cannot target the caller's
    // dst directly under O_DIRECT.  AlignedBuffer's capacity is rounded up to
    // its alignment, which is exactly the guarantee the kernel requires.
    std::vector<AlignedBuffer> bounce_;

    // Per-op completion flags, parallel to ops_, so a reaped CQE cannot be
    // counted twice and poll() can report partial progress.
    std::vector<uint8_t> completed_;
    size_t remaining_ = 0;
    bool submitted_ = false;
    Status first_error_;
};

} // namespace ANCHORDB_NAMESPACE
