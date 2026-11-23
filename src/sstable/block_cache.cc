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

#include "sstable/block_cache.h"
#include "anchordb/env.h"
#include "cache/cache_wrapper.h"
#include "util/coding.h"
#include <cstring>
#include <string_view>
#include <utility>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

namespace {

// Approximate fixed overhead of one resident block beyond its byte payload and
// parsed entries.  Used only for cache charging.
constexpr size_t kCachedBlockOverhead = 64;

// Decodes the stored block bytes, decompresses them when a codec is active, and
// returns the uncompressed block in *out.  Every length read from the block or
// the codec is validated before a buffer is resized.
Status read_and_decode_block(RandomAccessFileReader *reader, uint64_t file_size, const Compressor &codec,
                             uint64_t block_offset, uint32_t stored_size, std::string *out) {
    *out = std::string();
    if (reader == nullptr) { return Status::invalid_argument("block cache: null reader"); }
    if (block_offset > file_size || stored_size > file_size - block_offset) {
        return Status::corruption("block cache: data block out of range", std::to_string(block_offset));
    }
    if (stored_size < kDataBlockHeaderSize) {
        return Status::corruption("block cache: data block too small", std::to_string(stored_size));
    }

    std::string stored;
    stored.resize(stored_size);
    std::string_view view;
    Status s = reader->read(block_offset, stored_size, &view, stored.data());
    if (!s.is_ok()) { return s; }
    if (view.size() < stored_size) {
        return Status::corruption("block cache: short data block read", std::to_string(view.size()));
    }
    if (view.data() != stored.data()) { std::memmove(stored.data(), view.data(), stored_size); }

    uint32_t uncompressed_size = 0;
    std::string_view payload;
    if (!parse_data_block_header(stored, &uncompressed_size, &payload)) {
        return Status::corruption("block cache: missing data block header");
    }
    if (uncompressed_size == 0 || uncompressed_size > kMaxDataBlockSize) {
        return Status::corruption("block cache: invalid data block length", std::to_string(uncompressed_size));
    }

    // A zero codec id means the block was stored verbatim after its header.
    if (codec.id == 0 || codec.uncompress == nullptr) {
        if (payload.size() != uncompressed_size) {
            return Status::corruption("block cache: data block length mismatch", std::to_string(payload.size()));
        }
        out->assign(payload.data(), payload.size());
        return Status::ok();
    }
    if (codec.get_uncompressed_length == nullptr) {
        return Status::corruption("block cache: codec lacks length reporting");
    }

    size_t reported_size = 0;
    if (!codec.get_uncompressed_length(payload.data(), payload.size(), &reported_size)) {
        return Status::corruption("block cache: bad compressed data block");
    }
    if (reported_size != uncompressed_size) {
        return Status::corruption("block cache: data block length mismatch", std::to_string(reported_size));
    }

    out->resize(uncompressed_size);
    if (!codec.uncompress(payload.data(), payload.size(), out->data())) {
        return Status::corruption("block cache: data block decompression failed");
    }
    return Status::ok();
}

// Builds the resident entry for an already-decoded block.  Returns null (with
// *s set) when the bytes do not parse.
CachedDataBlock *make_block(std::string *bytes, Status *s) {
    CachedDataBlock *block = new CachedDataBlock();
    block->bytes = std::move(*bytes);
    block->reader = new DataBlockReader(block->bytes);
    if (!block->reader->valid()) {
        *s = Status::corruption("block cache: invalid data block");
        delete block->reader;
        delete block;
        return nullptr;
    }
    *s = Status::ok();
    return block;
}

} // namespace

void BlockCache::encode_key(uint64_t block_offset, uint32_t stored_size, char *key) const {
    key_namespace_.write_prefix(key);
    char *fields = key + CacheKeyNamespace::kPrefixSize;
    encode_fixed_64(fields, block_offset);
    encode_fixed_32(fields + sizeof(uint64_t), stored_size);
}

void BlockCache::destroy_entry(CachedDataBlock *block) {
    if (block == nullptr) { return; }
    delete block->reader;
    delete block;
}

void BlockCache::delete_entry(std::string_view, void *value) { destroy_entry(static_cast<CachedDataBlock *>(value)); }

Status BlockCache::get(RandomAccessFileReader *reader, uint64_t file_size, const Compressor &codec,
                       uint64_t block_offset, uint32_t stored_size, Cache::Handle *pin, CachedDataBlock **block) const {
    if (pin == nullptr || block == nullptr) { return Status::invalid_argument("block cache: null block output"); }
    if (cache_ == nullptr) { return Status::invalid_argument("block cache: no cache"); }
    pin->reset();
    *block = nullptr;

    // Fixed-width key: namespace | block offset | stored size.
    char key[CacheKeyNamespace::kPrefixSize + sizeof(uint64_t) + sizeof(uint32_t)];
    encode_key(block_offset, stored_size, key);
    const std::string_view key_view(key, sizeof(key));

    auto open = [&](CachedDataBlock **out) {
        std::string bytes;
        Status s = read_and_decode_block(reader, file_size, codec, block_offset, stored_size, &bytes);
        if (!s.is_ok()) { return s; }
        *out = make_block(&bytes, &s);
        return s;
    };
    auto charge = [](CachedDataBlock *opened) {
        return kCachedBlockOverhead + opened->bytes.capacity() + opened->reader->size() * sizeof(DataBlockEntry);
    };

    // Every value that reaches the cache is owned by it, and every pin keeps
    // its block alive, so the caller never frees a block on any path.  A
    // zero-capacity cache declines the insert but still hands back a pin on an
    // uncached entry (cache_lookup_or_open frees the opened value itself).
    CacheWrapper wrapper(cache_);
    return cache_lookup_or_open<CachedDataBlock>(wrapper, key_view, pin, block, open, charge, &delete_entry);
}

} // namespace ANCHORDB_NAMESPACE
