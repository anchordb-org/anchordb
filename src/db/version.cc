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

#include "db/version.h"

#include <utility>

#include "db/version_set.h"

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

Version::Version(VersionSet *owner, std::vector<std::vector<FileMetaData>> levels, std::vector<uint32_t> vlog_segments)
    : owner_(owner), refs_(1), levels_(std::move(levels)), vlog_segments_(std::move(vlog_segments)) {}

Version::~Version() = default;

void Version::unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) { owner_->unref_version_(this); }
}

} // namespace ANCHORDB_NAMESPACE
