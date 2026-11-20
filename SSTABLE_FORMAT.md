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

  | Value  | Name        | Meaning                           |
  | ------ | ----------- | --------------------------------- |
  | `0x00` | `Tombstone` | Key deletion marker               |
  | `0x01` | `Inline`    | Value stored inline in `-Data.adb` |
  | `0x02` | `Blob`      | Value stored in a vLog file       |

### File overview

| File           | Contents                                                            |
| -------------- | ------------------------------------------------------------------- |
| `-Data.adb`    | Concatenated data records (inline values and tombstones)            |
| `-Index.adb`   | Concatenated fixed-size index-block segments, optionally compressed |
| `-Summary.adb` | Top-level sparse index: one entry per index block                   |
| `-Filter.adb`  | Per-table membership filter                                         |
| vLog files     | Concatenated data records whose `EntryType` is `Blob`               |

`-Data.adb` and the vLog use the **same record format**. An index slot addresses
an inline record by `(offset, size)` into `-Data.adb`, and a blob's index slot
addresses the complete vLog record by `(file_id, record_offset, record_size)`.
In both cases `offset` points at the start of the record header and `size` is
the full encoded record size.

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

| Field         | Width        | Description                                             |
| ------------- | ------------ | ------------------------------------------------------- |
| `crc32`       | 4 B          | Masked CRC32 over every byte after this field           |
| `timestamp`   | 8 B          | `uint64_t`, record timestamp                            |
| `type`        | 1 B          | `EntryType` (`0x0` tombstone, `0x1` inline, `0x2` blob) |
| `key_size`    | 2 B          | `uint16_t`, byte length of `key_bytes`                  |
| `key_bytes`   | `key_size`   | User key                                                |
| `value_size`  | 1–5 B        | `varint32`, byte length of `value_bytes`                |
| `value_bytes` | `value_size` | Raw value (for a vLog record, the blob payload)         |

Notes:

- The CRC is computed over `timestamp` through the end of `value_bytes`
  (everything after the CRC field) and then written into the leading field.
- `key_size` is limited to `kDataRecordMaxKeySize = 65535`.
- `value_size` is limited to `UINT32_MAX`.
- Encoded length:
  `13 + 2 + key_size + varint_len(value_size) + value_size`.
- Records are concatenated with no padding; a reader walks them by decoding one
  record and advancing by its encoded length.

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
 |   slot[0]   : 19 bytes (detailed layout below)             |
 |   slot[1]   : 19 bytes                                     |
 |   ...                                                      |
 |   slot[N-1] : 19 bytes                                     |
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

### Forward slot (19 bytes)

Slots are stored in ascending key order, immediately after the header. Slot `i`
starts at block-local offset `8 + i * 19`.

```
 slot (19 bytes), offsets relative to the start of the slot
 +------------------+------+---------+--------+----------------------+
 | key_local_offset | type | file_id |  size  |  data_global_offset  |
 |     2 bytes      | 1 B  |  4 B    |  4 B   |        8 B           |
 +------------------+------+---------+--------+----------------------+
 0                  2      3         7        11                     19
```

| Offset | Width | Field                | Description                                                                     |
| ------ | ----- | -------------------- | ------------------------------------------------------------------------------- |
| 0      | 2     | `key_local_offset`   | Block-local offset of the payload entry (`key_len` field)                       |
| 2      | 1     | `type`               | `EntryType`                                                                     |
| 3      | 4     | `file_id`            | vLog file id for `Blob`; always 0 for inline/tombstone                          |
| 7      | 4     | `size`               | Full encoded record size for inline and blob entries                            |
| 11     | 8     | `data_global_offset` | Inline: absolute offset in `-Data.adb`. Blob: absolute record offset in the vLog |

The fields above form `ValueHandle { type, file_id, offset, size }`.
For inline entries `(data_global_offset, size)` covers the whole encoded data
record in `-Data.adb`. For blob entries `(file_id, data_global_offset, size)`
covers the whole encoded record in the vLog, starting at its header.

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
| `kIndexSlotSize`       | 19 B                           | Fixed slot size        |
| `kDefaultBlockSize`    | 4096 B                         | Default segment size   |
| `kIndexBlockAlignment` | 4096 B                         | Segment size alignment |
| `kMinIndexBlockSize`   | 4096 B                         | Minimum segment size   |
| `kMaxIndexBlockSize`   | 65536 B                        | Maximum segment size   |
| effective max key size | `min(requested, block_size/4)` | Enforced by the writer |

- `block_size` is clamped to `[4 KiB, 64 KiB]` and rounded up to a 4 KiB
  multiple. The uncompressed segment is always exactly this size.
- `key_local_offset` is 16-bit, so every payload offset (and therefore the whole
  addressable segment) must fit in `uint16_t`.
- An entry consumes `19 + 2 + key_size + 8` bytes (slot plus payload). The block
  is sealed when the next entry would not fit or would push
  `free_space_boundary` beyond `UINT16_MAX`.
- On disk, a block may be compressed. The summary records the stored
  (possibly compressed) size, and the summary header records the compressor id.
  Compression is applied **per block**; `-Index.adb` itself has no other framing.

### Index iterator value (in-memory only)

`IndexBlockReader::new_iterator()` exposes each entry's value as a packed
25-byte record (not stored in this form on disk):

```
 +----------+-----------+----------+--------------+-------------+
 | type (1B)| file_id(4)| size (4) | offset (8B)  | timestamp(8)|
 +----------+-----------+----------+--------------+-------------+
```

---

## 3. Summary Index

`-Summary.adb` is the sparse top-level index: one entry per index block, mapping
the block's greatest key to its location in `-Index.adb`. Layout and code are in
`src/sstable/summary.{h,cc}`.

```
 +----------------------+--------------------------+
 | block_size (4 B)     | compressor_id (4 B)      |   Header (8 B)
 +----------------------+--------------------------+
 | entry 0                                        |
 | entry 1                                        |
 | ...                                            |
 +------------------------------------------------+
```

### Header (8 bytes)

| Offset | Width | Field           | Description                                             |
| ------ | ----- | --------------- | ------------------------------------------------------- |
| 0      | 4     | `block_size`    | Index-block segment size for the whole table            |
| 4      | 4     | `compressor_id` | Codec used for every index block; `0` = stored verbatim |

A zero `block_size` marks the summary as malformed.

### Entry

Entries are appended in ascending key order.

| Width     | Field             | Description                                               |
| --------- | ----------------- | --------------------------------------------------------- |
| 4 B       | `key_len`         | `uint32_t` length of the separator key                    |
| `key_len` | `key_bytes`       | Greatest key in the referenced index block                |
| 8 B       | `block_offset`    | Absolute byte offset of the block in `-Index.adb`          |
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
