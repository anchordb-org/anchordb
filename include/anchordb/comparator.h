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

#include "anchordb/export.h"

#include "anchordb/anchordb_namespace.h"
#include <string_view>

namespace ANCHORDB_NAMESPACE {

// Ordering definition for user keys, expressed as plain function pointers so a
// custom comparator needs no inheritance.
//
// A comparator is a pure 3-way comparison over raw key bytes. All keys in a database must be
// ordered consistently by the comparator that created it, and that comparator
// must be supplied again when the database is reopened.
struct ANCHORDB_EXPORT Comparator {
    // Returns < 0 if a < b, 0 if a == b, > 0 if a > b.  Must be a strict weak
    // ordering.
    int (*compare)(std::string_view a, std::string_view b) = nullptr;

    // Returns a stable, human-readable name for the comparator (used for
    // diagnostics and to detect a comparator mismatch).
    const char *(*name)() = nullptr;
};

// Returns a comparator that orders keys by their raw bytes.
ANCHORDB_EXPORT const Comparator new_byte_wise_comparator();

} // namespace ANCHORDB_NAMESPACE
