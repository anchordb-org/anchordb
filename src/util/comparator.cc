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

#include "anchordb/comparator.h"
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {
int byte_wise_compare(std::string_view a, std::string_view b) { return a.compare(b); }

const char *byte_wise_name() { return "anchordb.ByteWiseComparator"; }
} // namespace

const Comparator new_byte_wise_comparator() { return Comparator{&byte_wise_compare, &byte_wise_name}; }

} // namespace ANCHORDB_NAMESPACE
