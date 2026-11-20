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
#include "db/file_metadata.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace ANCHORDB_NAMESPACE {

class VersionSet;

// Immutable snapshot of the SSTable level layout and the vLog segments that
// snapshot depends on.
//
// A Version never changes after construction.  The VersionSet owns one
// reference to the current Version; a reader, iterator or compaction pins a
// Version with ref() and releases it with unref().  A Version that has been
// replaced as current stays alive while pinned, so files referenced by an
// in-flight read are not deleted underneath it.
//
// Manual reference counting, matching MemTable/TableReader.  ref/unref are
// atomic, but a Version must be installed or released with VersionSet
// operations serialized by the caller (the DB layer's lock).
class Version {
  public:
    Version(const Version &) = delete;
    Version &operator=(const Version &) = delete;

    void ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void unref();

    // Live tables per level.  L0 is newest first; L1+ are ordered by
    // smallest_key then table id.
    const std::vector<std::vector<FileMetaData>> &levels() const { return levels_; }
    const std::vector<FileMetaData> &level_files(uint32_t level) const { return levels_[level]; }

    // vLog segment file ids this Version may still reference.
    const std::vector<uint32_t> &vlog_segments() const { return vlog_segments_; }

  private:
    friend class VersionSet;

    Version(VersionSet *owner, std::vector<std::vector<FileMetaData>> levels, std::vector<uint32_t> vlog_segments);
    ~Version();

    VersionSet *owner_;
    std::atomic<int> refs_;
    std::vector<std::vector<FileMetaData>> levels_;
    std::vector<uint32_t> vlog_segments_;

    // Intrusive list of all live (current or pinned) Versions.
    Version *next_ = nullptr;
    Version *prev_ = nullptr;
};

} // namespace ANCHORDB_NAMESPACE
