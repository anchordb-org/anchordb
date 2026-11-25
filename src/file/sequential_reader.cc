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

#include "anchordb/anchordb_namespace.h"
#include "file_meta.h"
#include "port/port.h" // IWYU pragma: keep
#include "sequential_reader.h"
#include "sync_io.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace ANCHORDB_NAMESPACE {

namespace {

constexpr uintptr_t kAlignMask = static_cast<uintptr_t>(kDirectIOAlignment) - 1;

bool is_aligned(uintptr_t x) { return (x & kAlignMask) == 0; }

} // namespace

Status PosixSequentialFileReader::refill_buffer_() {
    if (buf_.capacity() == 0) {
        return Status::io_error(std::string_view("sequential reader: O_DIRECT bounce buffer is empty"), filename_);
    }

    // Read the block-aligned window that contains offset_, so the bytes at
    // offset_ land somewhere in [buf_file_offset_, buf_file_offset_ + valid).
    const uint64_t aligned_offset = offset_ & ~static_cast<uint64_t>(kDirectIOAlignment - 1);
    size_t got = 0;
    Status s = file_internal::sync_pread(filename_, fd_, buf_.data(), buf_.capacity(), aligned_offset, &got);
    if (!s.is_ok()) { return s; }

    buf_file_offset_ = aligned_offset;
    buf_bytes_valid_ = got;
    return Status::ok();
}

void PosixSequentialFileReader::advise_sequential_() {
    if (advised_) { return; }
    advised_ = true;
    // Advisory only; ignore the result (ENOSYS on old kernels, ENOTSUP on some
    // filesystems).  A larger readahead window is all we are asking for.
    (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
}

void PosixSequentialFileReader::prefetch_() {
    // Start populating the page cache for the next window; read(2) will then
    // find it resident instead of blocking on each 4K page.
    (void)::posix_fadvise(fd_, static_cast<off_t>(offset_), static_cast<off_t>(kSeqFileReaderBufSize),
                          POSIX_FADV_WILLNEED);
}

Status PosixSequentialFileReader::read(size_t n, std::string_view *result, char *scratch) {
    *result = std::string_view();
    if (n == 0) { return Status::ok(); }

    if (!direct_io_) { advise_sequential_(); }

    size_t total = 0;

    while (total < n) {
        // Serve whatever the bounce window already holds at offset_.
        if (offset_ >= buf_file_offset_ && offset_ < buf_file_offset_ + buf_bytes_valid_) {
            const size_t in_buf = static_cast<size_t>(offset_ - buf_file_offset_);
            const size_t take = std::min(n - total, buf_bytes_valid_ - in_buf);

            std::memcpy(scratch + total, buf_.data() + in_buf, take);
            total += take;
            offset_ += take;
            continue;
        }

        // Buffered path: read(2) lets the kernel keep the file position and
        // drive readahead; skip() moves that position with lseek().
        if (!direct_io_) {
            size_t got = 0;
            Status s = file_internal::sync_read(filename_, fd_, scratch + total, n - total, &got);
            if (!s.is_ok()) { return s; }

            if (got == 0) { break; } // end of file

            total += got;
            offset_ += got;
            prefetch_();
            continue;
        }

        // O_DIRECT fast path: an aligned request of at least one buffer's
        // worth is read straight into the caller, skipping the bounce buffer.
        const size_t want = n - total;
        char *const dst = scratch + total;
        if (buf_.capacity() != 0 && want >= buf_.capacity() && is_aligned(offset_) &&
            is_aligned(reinterpret_cast<uintptr_t>(dst))) {
            // O_DIRECT needs a block-multiple length; the sub-block remainder,
            // if any, is picked up by the bounce path on the next iteration.
            const size_t len = want & ~static_cast<size_t>(kDirectIOAlignment - 1);
            size_t got = 0;
            Status s = file_internal::sync_pread(filename_, fd_, dst, len, offset_, &got);
            if (!s.is_ok()) { return s; }

            total += got;
            offset_ += got;
            buf_bytes_valid_ = 0;     // the stale window, if any, now lies behind us
            if (got < len) { break; } // short aligned read means end of file
            continue;
        }

        // O_DIRECT fallback: stage a block-aligned window through buf_ and
        // loop back to copy out of it.  This is the only path that handles
        // unaligned offsets, unaligned destinations, and small reads.
        Status s = refill_buffer_();
        if (!s.is_ok()) { return s; }

        // If the refilled window does not reach offset_ it is because the file
        // ended first; there is nothing more to read.
        if (offset_ >= buf_file_offset_ + buf_bytes_valid_) { break; }
    }

    *result = std::string_view(scratch, total);
    return Status::ok();
}

Status PosixSequentialFileReader::skip(uint64_t n) {
    offset_ += n;

    if (offset_ < buf_file_offset_ || offset_ >= buf_file_offset_ + buf_bytes_valid_) { buf_bytes_valid_ = 0; }

    if (!direct_io_) {
        // The buffered path reads through the kernel file position, so keep it
        // in step with offset_.
        if (::lseek(fd_, static_cast<off_t>(offset_), SEEK_SET) == static_cast<off_t>(-1)) {
            return posix_error(filename_, errno);
        }
    }

    return Status::ok();
}

} // namespace ANCHORDB_NAMESPACE
