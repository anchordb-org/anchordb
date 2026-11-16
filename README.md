# AnchorDB

> **Work in progress.** AnchorDB is under active development and is **not**
> ready for production use. The public API, on-disk formats, and build setup may
> change without notice.

AnchorDB is a from-scratch LSM-tree storage engine written in C++20, organized
as a WiscKey-style key-value/blob store. It follows the LevelDB/RocksDB lineage:
an in-memory skiplist memtable, a write-ahead log, SSTables on disk, block
compression, per-table filters, and a separate value log for large values.

## Status

The storage primitives are largely implemented:

- Arena-backed skiplist memtable
- vLog-backed memtable writes and blob reads: values above `value_threshold`
  are appended to the vLog, and `MemTable::get()` / `MemtableIterator` resolve
  blob handles back through the `Env`
- Write-ahead log (`src/db/wal_*`)
- SSTable data records, index blocks, summary index, and per-table filters
  (`src/sstable/`)
- Table reader, table iterator, and open-table cache (`src/sstable/table*.{h,cc}`)
- Block/table caches: LRU, LIRS, CLOCK-Pro, ARC
- Bloom, cuckoo, and ribbon filters
- Environment and file layers, with optional `io_uring` and `O_DIRECT`
- Compression codecs: zstd, snappy, lz4, and zlib

The layers that tie them together are still being built:

- Compaction
- SSTable blob read path (`Table` still rejects `EntryType::Blob` entries)
- vLog garbage collection
- Versioning / MANIFEST
- Recovery and snapshots
- Database layer and public `db.h`
- Merge iterator
- Tests and benchmarks

Blob values are readable through the memtable: when `env` and `vlog_dbname` are
supplied, `MemTable::get()` and `MemtableIterator` resolve a `BlobHandle` by
reading and decoding the referenced vLog record. The SSTable read path does not
resolve blobs yet: `Table::get()` returns `not_supported` for `EntryType::Blob`
and the table iterator surfaces an error for them. vLog garbage collection is
not implemented. There is no test suite or benchmark target, and the API should
be considered unstable.

## Requirements

- A C++20 compiler
- CMake 3.23+
- [vcpkg](https://github.com/microsoft/vcpkg) checked out at `$HOME/vcpkg` for
  the bundled preset. `vcpkg.json` requests `zstd`, `snappy`, `lz4`, `zlib`, and
  `gperftools`.
- Linux/Unix: `liburing` is **optional**. Without it the build falls back to
  synchronous I/O.
- `tcmalloc` (`gperftools`) is optional; without it the system allocator is used.

Windows builds use IOCP instead of `io_uring`/POSIX I/O.

## Build

```sh
cmake --preset default
cmake --build build -j
```

The preset configures a `Debug` build in `build/` with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`. The result is the static library
`build/libanchordb.a`.

The preset hardcodes
`CMAKE_TOOLCHAIN_FILE=$env{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake`; a
different vcpkg location requires overriding that cache variable.

### Build options

| Option                       | Default | Effect                                                                      |
| ---------------------------- | ------- | --------------------------------------------------------------------------- |
| `ANCHORDB_USE_IO_URING`      | `ON`    | Use `io_uring` on Linux when `liburing` is found; otherwise synchronous I/O |
| `ANCHORDB_DIRECT_IO_DEFAULT` | `ON`    | Open POSIX files with `O_DIRECT` by default                                 |
| `ANCHORDB_USE_TCMALLOC`      | `ON`    | Link tcmalloc process-wide; falls back to the system allocator              |

The compression codecs are detected individually with `find_package`. If a codec
is missing, the build still succeeds and that codec silently becomes a
pass-through. On x86-64 the build adds `-msse4.2 -mavx2`; IPO/LTO is enabled when
supported.

## Layout

- `include/anchordb/` — public headers (`options.h`, `env.h`, `comparator.h`,
  `compressor.h`, `filter.h`, `iterator.h`, `cache.h`, `status.h`)
- `src/` — implementation, grouped by subsystem:
  - `cache/` — block/table cache policies and hash-map backends
  - `db/` — internal key format, WAL, iterators
  - `env/` — platform environment and POSIX I/O
  - `file/` — file naming, readers, writers, locking, and rate limiting
  - `memory/` — arena allocator
  - `memtable/` — skiplist and memtable table
  - `port/` — platform and portability definitions (including Windows IOCP)
  - `sstable/` — data records, index blocks, summary, filters, tables, and
    table cache
  - `util/` — coding, CRC32, hashing, compression, filters, and logging

## Documentation

- [`SSTABLE_FORMAT.md`](SSTABLE_FORMAT.md) — binary layout of data records,
  index blocks, summary index, and filter blocks
- [`AGENTS.md`](AGENTS.md) — contributor and agent instructions

## License

Apache-2.0. See [`LICENSE`](LICENSE) for details.
