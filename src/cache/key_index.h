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

#include "anchordb/options.h"
#include "cache/linear_prob_map.h"
#include "cache/robinhood_map.h"
#include "cache/swiss_map.h"
#include "cache/util.h"
#include <cstddef>
#include <variant>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

// Key -> entry index for a cache shard, dispatching to the backend selected by
// MapType.  This is the single place that knows the std::variant, so policies
// call get/insert/remove directly instead of repeating std::visit.
template <typename T> class KeyIndex {
  public:
    KeyIndex() = default;

    KeyIndex(size_t capacity, MapType type) {
        switch (type) {
        case MapType::LinearProbing: map_.template emplace<LinearProbingMap<T>>(capacity); break;
        case MapType::RobbinHood: map_.template emplace<RobinHoodMap<T>>(capacity); break;
        case MapType::Swiss: map_.template emplace<SwissMap<T>>(capacity); break;
        }
    }

    bool contains(HashedKey key) const {
        return std::visit([&](const auto &active_map) { return active_map.contains(key); }, map_);
    }

    T get(HashedKey key) const {
        return std::visit([&](const auto &active_map) { return active_map.get(key); }, map_);
    }

    void insert(HashedKey key, T value) {
        std::visit([&](auto &active_map) { active_map.insert(key, value); }, map_);
    }

    T remove(HashedKey key) {
        return std::visit([&](auto &active_map) { return active_map.remove(key); }, map_);
    }

    void clear() {
        std::visit([](auto &active_map) { active_map.clear(); }, map_);
    }

  private:
    std::variant<LinearProbingMap<T>, RobinHoodMap<T>, SwissMap<T>> map_;
};

} // namespace ANCHORDB_NAMESPACE
