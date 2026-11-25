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
#include "file_meta.h"
#include "port/port.h" // IWYU pragma: keep
#include "util/aligned_buffer.h"
#include <cstddef>
#include <unistd.h>

namespace ANCHORDB_NAMESPACE {

// Sequential reader backed by synchronous read(2)/pread(2).  Not thread-safe.
//
// With O_DIRECT, a request whose offset and destination are block-aligned and
// that spans at least one buffer is read straight into the caller's memory;
// every other request falls back through the aligned bounce buffer `buf`.  When
// `buf` is supplied it is used as that bounce buffer, otherwise one is
// allocated on demand.
class PosixSequentialFileReader final : public SequentialFileReader {
  public:
    PosixSequentialFileReader(std::string filename, int fd, AlignedBuffer *buf = nullptr, size_t buffer_size = 0,
                              bool direct_io = false)
        : fd_(fd), filename_(std::move(filename)), offset_(0), direct_io_(direct_io), buf_file_offset_(0),
          buf_bytes_valid_(0) {
        if (buf != nullptr) {
            buf_ = std::move(*buf);
        } else if (direct_io_) {
            size_t buf_size = (buffer_size != 0) ? buffer_size : kSeqFileReaderBufSize;
            buf_ = AlignedBuffer(buf_size, kDirectIOAlignment);
        }
    }

    ~PosixSequentialFileReader() override {
        if (fd_ >= 0) { ::close(fd_); }
    }

    Status read(size_t n, std::string_view *result, char *scratch) override;
    Status skip(uint64_t n) override;

  private:
    // Reads one block-aligned window into buf_ that covers offset_.  On return
    // buf_bytes_valid_ is 0 at end of file, and unchanged on error.
    Status refill_buffer_();

    // Tells the kernel the file is read sequentially (grows its readahead
    // window); done once, only for the buffered path.
    void advise_sequential_();
    // Asks the kernel to start reading the window that follows offset_; only
    // meaningful for the buffered path, since O_DIRECT bypasses the page cache.
    void prefetch_();

    const int fd_;
    const std::string filename_;
    uint64_t offset_;
    const bool direct_io_;

    AlignedBuffer buf_;
    uint64_t buf_file_offset_;
    size_t buf_bytes_valid_;
    bool advised_ = false;
};
} // namespace ANCHORDB_NAMESPACE
