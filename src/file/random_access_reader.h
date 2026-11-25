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
#include "limiter.h"
#include "port/port.h" // IWYU pragma: keep
#include "util/aligned_buffer.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unistd.h>

namespace ANCHORDB_NAMESPACE {

// Random-access reader.  Uses a permanent fd (owned and released to the limiter
// on destruction) when fd >= 0, otherwise opens the file per operation.
//
// read() is synchronous and uses pread(2).  queue_read() instead hands a read to
// the calling thread's ReadBatcher, which overlaps many files' reads in one
// submission; the caller runs execute() on that batcher.
class PosixRandomAccessFileReader final : public RandomAccessFileReader {
  public:
    // `env` supplies the batcher, resolved per call through
    // Env::read_batcher_for_thread(); it may be null, and queue_read() then
    // reports NotSupported so the caller reads synchronously.
    PosixRandomAccessFileReader(std::string filename, int fd, Limiter *limiter, Env *env, bool direct_io = true)
        : filename_(std::move(filename)), has_permanent_fd_(fd >= 0), fd_(fd), limiter_(limiter), env_(env),
          direct_io_(direct_io) {}

    ~PosixRandomAccessFileReader() override {
        if (has_permanent_fd_) {
            ::close(fd_);
            if (limiter_ != nullptr) { limiter_->release(); }
        }
    }

    // Reads up to `n` bytes at `offset` into `scratch` with pread(2) and points
    // `*result` at them; `*result` always refers to `scratch`, valid until the
    // next call.  Under O_DIRECT a non-aligned `offset`/`n`/`scratch` is serviced
    // through an aligned bounce buffer that is copied into `scratch` before
    // returning.  The read is capped at the end of the file, so a short result
    // means end of file, not an error.  Safe to call concurrently on one reader.
    Status read(uint64_t offset, size_t n, std::string_view *result, char *scratch) override;

    // Queues a read of `n` bytes at `offset` into `scratch` on the batcher
    // instead of doing it now.  The batcher writes the outcome into `*status`
    // and `*bytes_read` when the caller later calls execute() on it, so those
    // pointers and `scratch` must stay valid until then.  Returns NotSupported
    // when this reader has no batcher.
    Status queue_read(uint64_t offset, size_t n, char *scratch, Status *status, size_t *bytes_read,
                      bool *done = nullptr) override;

    // Reads `ops` (one region per element) as one batch.  Each op names an
    // offset, a length and a destination; this reader supplies its own
    // descriptor, so every op addresses this file.  Blocks until all the reads
    // complete.
    Status read_batch(std::span<BatchReadOp> ops) override;

    // The batcher queue_read() submits to, which the caller must execute() to
    // run the queued reads.  Resolved for the calling thread, so it is null
    // when this reader cannot batch.
    ReadBatcher *batcher() const override { return env_ != nullptr ? env_->read_batcher_for_thread() : nullptr; }

  private:
    // Opens the file for a one-shot operation when there is no permanent fd.
    Status open_transient_fd_(int *fd) const;

    const std::string filename_;
    const bool has_permanent_fd_;
    const int fd_;
    Limiter *const limiter_;
    Env *const env_;
    const bool direct_io_;

    // O_DIRECT reads that cannot target the caller's buffer bounce through an
    // aligned buffer, allocated per call or cached depending on
    // ANCHORDB_HAS_TCMALLOC:
    //
    //   tcmalloc  an aligned allocation is a per-thread size-class hit (~8 ns)
    //             that scales with threads, so a per-call buffer beats a shared
    //             one that must be held under a mutex across the pread.
    //   else      posix_memalign/free costs ~39 ns on glibc, so caching the
    //             buffer and paying for a mutex instead wins, except when many
    //             threads contend on the same reader.
    //
    // Both members are unused in the tcmalloc configuration.
    std::mutex mu_;
    AlignedBuffer aligned_buf_;
};

} // namespace ANCHORDB_NAMESPACE
