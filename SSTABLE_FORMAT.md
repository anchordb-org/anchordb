# SSTable On-Disk Format

Binary layout of the SSTable data path (`-Data.adb`, `-Index.adb`,
`-Summary.adb`, `-Filter.adb`) and the WiscKey value log (`-Vlog.adb`). This
document is a specification of the current implementation; the normative source
of truth is the code written throughout the codebase.

## Conventions

- All fixed-width integers are **little-endian** (`encode_fixed_16/32/64` in
  `src/util/coding.h`).
- `varint` means unsigned LEB128 (little-endian base-128, 7 data bits per byte,
  MSB is the continuation flag).
- Sizes are byte counts. Offsets are absolute within the named file unless
  explicitly described as block-local.
- Keys stored in `-Data.adb`, `-Index.adb`, and `-Summary.adb` are **user keys**;
  the 8-byte internal-key trailer is not used in these files.
- `EntryType` is a single byte:

  | Value  | Name         | Meaning                                                          |
  | ------ | ------------ | ---------------------------------------------------------------- |
  | `0x00` | `Tombstone`  | Key deletion marker                                              |
  | `0x01` | `Inline`     | Value stored inline in `-Data.adb`                               |
  | `0x02` | `Blob`       | Verbatim value stored in a vLog file                             |
  | `0x03` | `Compressed` | vLog-only record whose value bytes are a compressed blob payload |

  `Compressed` is valid only inside a vLog file. The memtable, `-Data.adb`, and
  SSTable index slots use `Inline`, `Tombstone`, or `Blob` only; a blob's index
  slot always carries a `BlobHandle`, and whether the referenced vLog record is
  compressed is internal to the vLog. The writer falls back to `Blob` whenever
  compression would not shrink a value.

### File overview

| File           | Contents                                                            |
| -------------- | ------------------------------------------------------------------- |
| `-Data.adb`    | Framed, optionally compressed blocks of inline/tombstone records    |
| `-Index.adb`   | Concatenated fixed-size index-block segments, optionally compressed |
| `-Summary.adb` | Top-level sparse index: one entry per index block                   |
| `-Filter.adb`  | Per-table membership filter                                         |
| vLog files     | Concatenated data records whose `EntryType` is `Blob`               |

`-Data.adb` and the vLog use the **same record format**. An inline index slot
addresses a framed data block by `(block_offset, stored_size)` plus the record's
`in_block_offset`; the record header starts at that in-block offset. A blob's
index slot addresses the complete vLog record by
`(file_id, record_offset, record_size)`.

### vLog file header

Every vLog file starts with a fixed 16-byte header before its first record. It
records the codec used by `Compressed` records in that file, so changing the
codec option never makes an existing vLog unreadable. Blob records stored
verbatim may coexist with compressed records in the same file.

```
 offset 0
 +-----------+-------------+------------+------------------+
 | magic (4B)| version (4B)| codec (4B) | masked crc (4B)  |
 | "ANVL"    | 1           | Compressor::id (0 = verbatim)    |
 +-----------+-------------+------------+------------------+
 |<--------- CRC-covered region --------->|
```

- The masked CRC32 covers the magic, version, and codec fields.
- `codec` is a `Compressor::id` (for example `0x2001` for Zstd); `0` means no
  codec is configured for the file, and a `Compressed` record is then invalid.
- Readers load this header once when they open the file and cache the resolved
  codec alongside the descriptor; record resolution never re-reads it.
- The first record starts at `kVLogFileHeaderSize` (16). `VersionSet` vLog
  head/tail offsets are physical file offsets and therefore account for it.

---

## 1. Data Record

Used by `-Data.adb` and the WiscKey value log. Encode/decode lives in
`src/sstable/data_record.{h,cc}` and is shared by `RecordWriter`,
`RecordReader`, and `RecordBufferReader`.

```
 offset 0
 +------------+----------------+-----------+---------------+-----------+---------------------+-------------+
 | crc32 (4B) | timestamp (8B) | type (1B) | key_size (2B) | key_bytes | value_size (varint) | value_bytes |
 +------------+----------------+-----------+---------------+-----------+---------------------+-------------+
 |<---------------------------- CRC-covered region ------------------------------------------------------>|
```

| Field         | Width        | Description                                                                    |
| ------------- | ------------ | ------------------------------------------------------------------------------ |
| `crc32`       | 4 B          | Masked CRC32 over every byte after this field                                  |
| `timestamp`   | 8 B          | `uint64_t`, record timestamp                                                   |
| `type`        | 1 B          | `EntryType` (`0x0` tombstone, `0x1` inline, `0x2` blob, `0x3` compressed blob) |
| `key_size`    | 2 B          | `uint16_t`, byte length of `key_bytes`                                         |
| `key_bytes`   | `key_size`   | User key                                                                       |
| `value_size`  | 1–5 B        | `varint32`, byte length of `value_bytes`                                       |
| `value_bytes` | `value_size` | Raw value, or the compressed payload for `0x3`                                 |

Notes:

- The CRC is computed over `timestamp` through the end of `value_bytes`
  (everything after the CRC field) and then written into the leading field.
- `key_size` is limited to `kDataRecordMaxKeySize = 65535`.
- `value_size` is limited to `UINT32_MAX`.
- Encoded length:
  `13 + 2 + key_size + varint_len(value_size) + value_size`.
- For `0x3`, `value_size` and `value_bytes` describe the stored compressed
  payload; the CRC covers those stored bytes, and the uncompressed length comes
  from the codec in the vLog file header.
- Records are concatenated with no padding; a reader walks them by decoding one
  record and advancing by its encoded length.

---

## 1a. Data Blocks

`-Data.adb` is a run of whole-record blocks. `DataBlockBuilder` groups records
until the target size is reached (a block is never split inside a record and may
exceed the target by one record). `TableBuilder` compresses each block with the
table's data codec and appends it with a 4-byte header:

```
 +--------------------------+----------------------------------+
 | uncompressed_size (4 B)  | stored block bytes               |
 +--------------------------+----------------------------------+
```

- `stored block bytes` are the codec output when the summary's
  `data_compressor_id` is nonzero, otherwise the plain block.
- Blocks are not padded; `stored_size` is carried by every index slot that points
  into the block, including the 4-byte header.
- The uncompressed block is a concatenation of data records (section 1), with no
  per-block record index: the index slot carries the record's in-block offset.
- `data_block_size` in the summary is the writer's target; it is not a hard
  decoded-size limit. Readers reject a decoded block larger than 64 MiB
  (`kMaxDataBlockSize`) to bound allocation.

---

## 2. Index Block

An index block is a fixed-size, self-contained segment. `-Index.adb` is a run of
these segments back-to-back from offset 0. Layout and code are in
`src/sstable/idx_block_writer.{h,cc}` and `idx_block_reader.{h,cc}`.

```
 +------------------------------------------------------------+
 | Header (8 B)                                               |
 |   slot_count          4 B  total slots in this block       |
 |   free_space_boundary 4 B  first byte of the payload       |
 |                            region (grows down from the end)|
 +------------------------------------------------------------+
 | Forward slot array (grows up from offset 8)                |
 |   slot[0]   : 15 bytes (detailed layout below)             |
 |   slot[1]   : 15 bytes                                     |
 |   ...                                                      |
 |   slot[N-1] : 15 bytes                                     |
 +------------------------------------------------------------+
 |                       free space                           |
 +------------------------------------------------------------+
 | Backward payload region (grows down from end of segment)   |
 |   entry[N-1] : [ key_len 2 B | key | timestamp 8 B ]       |
 |   ...                                                      |
 |   entry[0]   : [ key_len 2 B | key | timestamp 8 B ]       |
 +------------------------------------------------------------+
```

### Header (8 bytes)

| Offset | Width | Field                 | Description                                      |
| ------ | ----- | --------------------- | ------------------------------------------------ |
| 0      | 4     | `slot_count`          | Number of slots/entries in the block             |
| 4      | 4     | `free_space_boundary` | Block-local start of the backward payload region |

An empty block stores `free_space_boundary = block_size`.

### Forward slot (15 bytes)

Slots are stored in ascending key order, immediately after the header. Slot `i`
starts at block-local offset `8 + i * 15`.

```
 slot (15 bytes), offsets relative to the start of the slot
 +------------------+------+-------------------+--------+---------------+
 | key_local_offset | type | file_id/block_off |  size  |  data_offset  |
 |     2 bytes      | 1 B  |      4 B          |  4 B   |     4 B       |
 +------------------+------+-------------------+--------+---------------+
 0                  2      3                   7        11              15
```

| Offset | Width | Field                   | Description                                                                  |
| ------ | ----- | ----------------------- | ---------------------------------------------------------------------------- |
| 0      | 2     | `key_local_offset`      | Block-local offset of the payload entry (`key_len` field)                    |
| 2      | 1     | `type`                  | `EntryType`                                                                  |
| 3      | 4     | `file_id` / `block_offset` | Blob: vLog file id. Inline/tombstone: absolute data-block offset         |
| 7      | 4     | `size`                  | Inline/tombstone: stored data-block size. Blob: full record size             |
| 11     | 4     | `data_offset`           | Inline/tombstone: in-block record offset. Blob: absolute vLog record offset  |

The fields above form `ValueHandle { type, file_id, data_offset, size }`.
The 4-byte field and `data_offset` are interpreted by `type`:

- **Inline / Tombstone:** `file_id/block_offset` is the absolute offset of the
  framed data block in `-Data.adb`, `data_offset` is the record's byte offset
  inside the uncompressed block, and `size` is the stored block size.
- **Blob:** `file_id` is the vLog file id, and `data_offset`/`size` cover the
  whole encoded record in that vLog file, starting at its header.

Because a blob's absolute vLog offset is 32 bits, a vLog file is hard-capped at
4 GiB; the same ceiling applies to `-Data.adb` through its 32-bit block offset.
Writers reject an append that would cross the limit (see
`kMaxRecordFileSize`).

The memtable `BlobHandle` in `src/db/db_format.h` uses the same convention:
`(offset, size)` starts at the record header and covers the full encoded record.

### Backward payload entry

The payload grows downward from the end of the segment; each entry is:

```
 +---------------+-------------+----------------+
 | key_len (2B)  | key_bytes   | timestamp (8B) |
 +---------------+-------------+----------------+
```

`slot.key_local_offset` points at the `key_len` field.
`last_key()` returns the last added key, and the timestamp is stored with the
payload so the index block is self-describing.

### Sizing and constraints

| Constant               | Value                          | Meaning                |
| ---------------------- | ------------------------------ | ---------------------- |
| `kIndexHeaderSize`     | 8 B                            | Header size            |
| `kIndexSlotSize`       | 15 B                           | Fixed slot size        |
| `kDefaultBlockSize`    | 4096 B                         | Default segment size   |
| `kIndexBlockAlignment` | 4096 B                         | Segment size alignment |
| `kMinIndexBlockSize`   | 4096 B                         | Minimum segment size   |
| `kMaxIndexBlockSize`   | 65536 B                        | Maximum segment size   |
| effective max key size | `min(requested, block_size/4)` | Enforced by the writer |

- `block_size` is clamped to `[4 KiB, 64 KiB]` and rounded up to a 4 KiB
  multiple. The uncompressed segment is always exactly this size.
- `key_local_offset` is 16-bit, so every payload offset (and therefore the whole
  addressable segment) must fit in `uint16_t`.
- An entry consumes `15 + 2 + key_size + 8` bytes (slot plus payload). The block
  is sealed when the next entry would not fit or would push
  `free_space_boundary` beyond `UINT16_MAX`.
- On disk, a block may be compressed. The summary records the stored
  (possibly compressed) size, and the summary header records the compressor id.
  Compression is applied **per block**; `-Index.adb` itself has no other framing.

### Index iterator value (in-memory only)

`IndexBlockReader::new_iterator()` exposes each entry's value as a packed
21-byte record (not stored in this form on disk):

```
 +-----------+---------------------------+-----------+------------------+--------------+
 | type (1B) | file_id/block_offset (4B) | size (4B) | data_offset (4B) | timestamp(8B)|
 +-----------+---------------------------+-----------+------------------+--------------+
```

---

## 3. Summary Index

`-Summary.adb` is the sparse top-level index: one entry per index block, mapping
the block's greatest key to its location in `-Index.adb`. Layout and code are in
`src/sstable/summary.{h,cc}`.

```
 +-----------+-----------+--------------+----------------------+
 | magic (4B)| version(4B)| block_size(4B)| index_compressor(4B) |
 +-----------+-----------+--------------+----------------------+
 | data_compressor (4B)  | data_block_size (4B) | entry 0 ... |
 +------------------------------------------------------------+
```

### Header (24 bytes)

| Offset | Width | Field                 | Description                                              |
| ------ | ----- | --------------------- | -------------------------------------------------------- |
| 0      | 4     | `magic`               | `0x4D534E41` (`"ANSM"` little-endian)                    |
| 4      | 4     | `version`             | `2`                                                      |
| 8      | 4     | `block_size`          | Uncompressed index-block segment size                    |
| 12     | 4     | `index_compressor_id` | Codec for every index block; `0` = stored verbatim       |
| 16     | 4     | `data_compressor_id`  | Codec for every `-Data.adb` block; `0` = stored verbatim |
| 20     | 4     | `data_block_size`     | Uncompressed target size of a data block                 |

A zero `block_size` or `data_block_size` marks the summary as malformed.

### Entry

Entries are appended in ascending key order.

| Width     | Field             | Description                                               |
| --------- | ----------------- | --------------------------------------------------------- |
| 4 B       | `key_len`         | `uint32_t` length of the separator key                    |
| `key_len` | `key_bytes`       | Greatest key in the referenced index block                |
| 8 B       | `block_offset`    | Absolute byte offset of the block in `-Index.adb`         |
| 4 B       | `compressed_size` | On-disk byte length of the stored block at `block_offset` |

Readers seek by taking the first entry whose key is `>= target`. A missing entry
means the target is past the last block. Entries are not individually
checksummed.

---

## 4. Filter Block

`-Filter.adb` stores one membership filter for the whole table, built from the
distinct user keys in ascending order (consecutive duplicates dropped). Layout
and code are in `src/sstable/filter.{h,cc}`.

```
 +--------------+--------------+----------------+------------------+-------------------+
 | magic (4B)   | name_len (1B)| name (name_len)| key_count (4B)   | filter bytes ...  |
 | "ANDF"       |              |                |                  |                   |
 +--------------+--------------+----------------+------------------+-------------------+
```

| Field         | Width      | Description                                                  |
| ------------- | ---------- | ------------------------------------------------------------ |
| `magic`       | 4 B        | ASCII `ANDF`                                                 |
| `name_len`    | 1 B        | Length of the filter name, capped at 255                     |
| `name`        | `name_len` | `Filter::name()` of the implementation that built the filter |
| `key_count`   | 4 B        | Number of distinct keys fed to the filter                    |
| `filter data` | remainder  | Exact bytes produced by `Filter::create_filter()`            |

Notes:

- The fixed header size `kFilterBlockHeaderSize` is `4 + 1 + 4 = 9` bytes, but
  the `name` is variable-width and sits between `name_len` and `key_count`.
- A reader is valid only when the magic matches and the embedded name equals the
  currently configured filter's name. An invalid or mismatched filter is
  treated conservatively (`key_may_match` returns true).
- `key_count == 0` proves absence for every key; a non-empty count with no
  filter bytes also falls back to conservative `true`.
- When no filter is configured, no filter file is written.

---
