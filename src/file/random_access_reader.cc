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

#include "random_access_reader.h"
#include "anchordb/anchordb_namespace.h"
#include "file_meta.h"
#include "port/port.h" // IWYU pragma: keep
#include "port/port_posix.h"
#include "sync_io.h"
#include "util/aligned_buffer.h"
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <unistd.h>

namespace ANCHORDB_NAMESPACE {

namespace {

constexpr uintptr_t kAlignMask = static_cast<uintptr_t>(kDirectIOAlignment) - 1;

bool is_aligned(uintptr_t x) { return (x & kAlignMask) == 0; }

// Closes a transient fd when this scope ends; owns nothing otherwise.
struct FdCloser {
    int fd;
    bool owned;
    ~FdCloser() {
        if (owned && fd >= 0) { ::close(fd); }
    }
};

} // namespace

Status PosixRandomAccessFileReader::open_transient_fd_(int *fd) const {
    int flags = O_RDONLY | kOpenBaseFlags;
    if (direct_io_) { flags |= O_DIRECT; }

    const int opened = ::open(filename_.c_str(), flags, 0644);
    if (opened < 0) { return posix_error(filename_, errno); }
    *fd = opened;
    return Status::ok();
}

Status PosixRandomAccessFileReader::read(uint64_t offset, size_t n, std::string_view *result, char *scratch) {
    *result = std::string_view();

    int target_fd = fd_;
    if (!has_permanent_fd_) {
        Status s = open_transient_fd_(&target_fd);
        if (!s.is_ok()) { return s; }
    }
    const FdCloser closer{target_fd, !has_permanent_fd_};

    if (n == 0) { return Status::ok(); }

    // Do not clamp the extent to the file size: pread(2) stops at end of file,
    // so the caller gets a short result, and clamping an O_DIRECT extent to a
    // non-block-aligned file size would leave an unaligned read length.
    const size_t want = n;

    // Under O_DIRECT the kernel requires an aligned buffer, offset and length,
    // so a request that breaks any of those rules is bounced through an aligned
    // buffer and the caller's window is copied out afterwards.
    const bool needs_alignment =
        direct_io_ && (scratch == nullptr || !is_aligned(reinterpret_cast<uintptr_t>(scratch)) ||
                       !is_aligned(static_cast<uintptr_t>(offset)) || want % kDirectIOAlignment != 0);
    if (!needs_alignment) {
        size_t got = 0;
        Status s = file_internal::sync_pread(filename_, target_fd, scratch, want, offset, &got);
        if (!s.is_ok()) { return s; }
        *result = std::string_view(scratch, got);
        return Status::ok();
    }

    // Round the request out to whole blocks.  It may cross the end of file,
    // which only makes the kernel return a short read.
    const uint64_t read_offset = offset & ~static_cast<uint64_t>(kAlignMask);
    const size_t skip = static_cast<size_t>(offset - read_offset);
    const uint64_t end = static_cast<uint64_t>(offset + want);
    const uint64_t aligned_end = (end + kAlignMask) & ~static_cast<uint64_t>(kAlignMask);
    const size_t read_size = static_cast<size_t>(aligned_end - read_offset);

#if ANCHORDB_HAS_TCMALLOC
    // A buffer private to this call: with tcmalloc an aligned allocation is a
    // per-thread size-class hit that scales with threads, whereas a shared
    // buffer must be held under a mutex across the pread and collapses under
    // contention.
    AlignedBuffer bounce(read_size, kDirectIOAlignment);
    if (!bounce.is_allocated()) { return Status::io_error("read: cannot allocate aligned buffer", filename_); }

    size_t got = 0;
    Status s = file_internal::sync_pread(filename_, target_fd, bounce.data(), read_size, read_offset, &got);
    if (!s.is_ok()) { return s; }

    // Copy the window out: the buffer dies with this call, so the caller must not
    // be handed a view into it.
    const size_t payload = got > skip ? std::min(got - skip, want) : 0;
    if (payload > 0) { std::memcpy(scratch, bounce.data() + skip, payload); }
    *result = std::string_view(scratch, payload);
    return Status::ok();
#else
    // posix_memalign/free is ~39 ns on glibc, so a cached buffer beats spending
    // it per read.  The guard covers the pread and the copy out of aligned_buf_,
    // which is what makes this path safe for concurrent reads on one reader.
    std::lock_guard<std::mutex> lock(mu_);

    if (aligned_buf_.capacity() < read_size) { aligned_buf_.re_allocate(read_size); }
    if (!aligned_buf_.is_allocated()) { return Status::io_error("read: cannot allocate aligned buffer", filename_); }

    size_t got = 0;
    Status s = file_internal::sync_pread(filename_, target_fd, aligned_buf_.data(), read_size, read_offset, &got);
    if (!s.is_ok()) { return s; }

    // Same copy-out rule as above; the lock only needs to cover the bounce
    // buffer being reused, not the caller's scratch.
    const size_t payload = got > skip ? std::min(got - skip, want) : 0;
    if (payload > 0) { std::memcpy(scratch, aligned_buf_.data() + skip, payload); }
    *result = std::string_view(scratch, payload);
    return Status::ok();
#endif
}

Status PosixRandomAccessFileReader::queue_read(uint64_t offset, size_t n, char *scratch, Status *status,
                                               size_t *bytes_read, bool *done) {
    if (!has_permanent_fd_) { return Status::not_supported("queue_read: no permanent descriptor"); }
    ReadBatcher *b = batcher();
    if (b == nullptr) { return Status::not_supported("queue_read: no read batcher"); }

    if (!b->add(fd_, BatchReadOp{offset, n, scratch, status, bytes_read, done})) {
        return Status::not_supported("queue_read: batcher busy");
    }
    return Status::ok();
}

Status PosixRandomAccessFileReader::read_batch(std::span<BatchReadOp> ops) {
    if (ops.empty()) { return Status::ok(); }
    if (!has_permanent_fd_) { return Status::not_supported("read_batch: no permanent descriptor"); }
    ReadBatcher *b = batcher();
    if (b == nullptr) { return Status::not_supported("read_batch: no read batcher"); }

    // The batcher is shared with queue_read(), so this call may never partially
    // enqueue.  Only the first add() can reject (the generation is either busy
    // for all of them or accepting for all), so fail before touching the queue.
    if (!b->add(fd_, ops[0])) { return Status::not_supported("read_batch: batcher busy"); }
    for (size_t i = 1; i < ops.size(); i++) {
        if (!b->add(fd_, ops[i])) {
            // The contract above says this cannot happen, but if add() ever
            // gains another rejection path, run what we queued so the caller's
            // buffers are released instead of being stranded in the batcher.
            (void)b->execute();
            return Status::not_supported("read_batch: batcher busy");
        }
    }

    return b->execute();
}

} // namespace ANCHORDB_NAMESPACE
