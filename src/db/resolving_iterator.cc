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

#include "db/resolving_iterator.h"

#include "db/blob_resolver.h"
#include "db/entry_iterator.h"
#include <cassert>
#include <string>
#include <string_view>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

ResolvingIterator::ResolvingIterator(EntryIterator *inner, BlobResolver *resolver, Comparator comparator)
    : inner_(inner), resolver_(resolver), comparator_(comparator) {
    if (comparator_.compare == nullptr) { comparator_ = new_byte_wise_comparator(); }
}

ResolvingIterator::~ResolvingIterator() { delete inner_; }

bool ResolvingIterator::same_key(std::string_view a, std::string_view b) const {
    return comparator_.compare(a, b) == 0;
}

void ResolvingIterator::reset_value() const {
    value_ready_ = false;
    value_view_ = std::string_view();
}

void ResolvingIterator::settle_forward() {
    reset_value();
    while (inner_ != nullptr && inner_->valid()) {
        if (inner_->type() != EntryType::Tombstone) {
            valid_ = true;
            return;
        }
        // The newest version of this key is a tombstone, so the whole key is
        // deleted: skip every remaining version of it.
        std::string dead_key(inner_->key());
        do { inner_->next(); } while (inner_->valid() && same_key(inner_->key(), dead_key));
    }
    valid_ = false;
}

void ResolvingIterator::settle_backward() {
    reset_value();
    while (inner_ != nullptr && inner_->valid()) {
        // Entries are ordered newest-first within a key, and a backward step can
        // land on an older version of a key.  Normalize to that key's newest
        // version before deciding whether it is visible.
        std::string current_key(inner_->key());
        inner_->seek(current_key);
        if (!inner_->valid()) {
            valid_ = false;
            return;
        }
        if (inner_->type() != EntryType::Tombstone) {
            valid_ = true;
            return;
        }
        // Deleted key: step to the previous key's oldest version and retry.
        inner_->prev();
    }
    valid_ = false;
}

void ResolvingIterator::seek_to_first() {
    status_ = Status::ok();
    if (inner_ == nullptr) {
        valid_ = false;
        return;
    }
    inner_->seek_to_first();
    settle_forward();
}

void ResolvingIterator::seek_to_last() {
    status_ = Status::ok();
    if (inner_ == nullptr) {
        valid_ = false;
        return;
    }
    inner_->seek_to_last();
    settle_backward();
}

void ResolvingIterator::seek(std::string_view target) {
    status_ = Status::ok();
    if (inner_ == nullptr) {
        valid_ = false;
        return;
    }
    inner_->seek(target);
    settle_forward();
}

void ResolvingIterator::next() {
    assert(valid_);
    reset_value();
    // Skip the remaining (older) versions of the key just yielded.
    std::string current_key(inner_->key());
    do { inner_->next(); } while (inner_->valid() && same_key(inner_->key(), current_key));
    settle_forward();
}

void ResolvingIterator::prev() {
    assert(valid_);
    reset_value();
    inner_->prev();
    settle_backward();
}

std::string_view ResolvingIterator::key() const {
    if (!valid_) { return std::string_view(); }
    return inner_->key();
}

std::string_view ResolvingIterator::value() const {
    if (!valid_) { return std::string_view(); }
    if (value_ready_) { return value_view_; }

    const ValueRef ref = inner_->value_ref();
    if (!inner_->status().is_ok()) {
        status_ = inner_->status();
        valid_ = false;
        return std::string_view();
    }

    if (is_vlog_blob_type(ref.type)) {
        if (resolver_ == nullptr) {
            status_ = Status::not_supported("resolving iterator: no blob resolver");
            valid_ = false;
            return std::string_view();
        }
        Status s = resolver_->resolve(ref.blob, inner_->key(), &value_buf_, &blob_scratch_);
        if (!s.is_ok()) {
            status_ = s;
            valid_ = false;
            return std::string_view();
        }
        value_view_ = value_buf_;
    } else {
        value_view_ = ref.inline_value;
    }

    value_ready_ = true;
    return value_view_;
}

Status ResolvingIterator::status() const {
    if (!status_.is_ok()) { return status_; }
    return inner_ != nullptr ? inner_->status() : Status::ok();
}

Iterator *new_resolving_iterator(EntryIterator *inner, BlobResolver *resolver, Comparator comparator) {
    if (inner == nullptr) { return new_error_iterator(Status::invalid_argument("resolving iterator: null inner")); }
    return new ResolvingIterator(inner, resolver, comparator);
}

} // namespace ANCHORDB_NAMESPACE
