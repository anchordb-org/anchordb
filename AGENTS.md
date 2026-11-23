# AGENTS.md

`anchordb` is a from-scratch C++20 LSM-tree storage engine (LevelDB/RocksDB lineage), organized as a WiscKey-style KV/blob store.

## Code discovery order

Look things up in this order and only fall through when the previous step has nothing:

1. **codebase-memory-mcp graph first.** The repo is indexed as project `home-tassok-anchordb`.
   - `search_graph(name_pattern=...)` to find a symbol; `get_code_snippet(qualified_name=...)` to read it.
   - `trace_path(function_name=..., direction="inbound"/"outbound"/"both")` for callers and callees.
   - `query_graph` (Cypher) for multi-hop or aggregate questions; `get_architecture` / `get_file_outline` for structure.
   - `detect_changes` to map a git diff to affected symbols.
2. **grep** when the graph returns nothing, or when the target is not code: string literals, error messages, config values, CMake, Markdown, and anything under an excluded path.
3. **find_path / list_directory** for filenames and layout.
4. **read_file** once the exact path is known.

Graph results are a best-effort index, not proof of absence. Known gaps in this repo:

- `src/env/` is **excluded by design** from the index, so `Env`, `PosixEnv`, and the io_uring code never appear in graph results — always grep there.
- `include/anchordb/{cache,comparator,compressor,env,filter}.h` are **partial-parse** files; constructs in the flagged ranges may be missing. Use grep/read for those headers.
- `.git`, `.cache`, `.zed`, and `build/` are also excluded, as are the gitignored notes listed under Status.

When the graph and the source disagree, trust the source, then say which one was stale.

## Status

- Pre-alpha. **No tests, benchmarks, lint, or typecheck commands exist.** The CMake test block is commented out at the end of `CMakeLists.txt` (`# enable_testing()`); do not claim a test suite or invent a test command.
- Do not add files under `src/` without also adding them to `target_sources(anchordb PRIVATE ...)` in `CMakeLists.txt`; otherwise they are silently never compiled.
- `readorder`, `.rules`, `.zed/`, and `ARCHITECTURE.md` are gitignored local-only notes. They may be absent or stale; verify against code/config.
- There is **no public `db.h` and no DB facade**: nothing opens a database, writes a memtable, or drives compaction. `include/anchordb/` holds only the primitive interfaces.
- Implemented under `src/db/` today: `db_format.h`, memtable `flush` (`flush.cc/.h`), `BlobResolver`, `ResolvingIterator`, `EntryIterator`, WAL (`wal.h`, `log.cc/.h`), `manifest.cc/.h`, and the full versioning stack (`version.h/.cc`, `version_edit.h/.cc`, `version_set.h/.cc`, `file_metadata.h`). Versioning/MANIFEST/recovery **do exist**, contrary to older notes.
- Still missing: compaction (no `Compaction`/`CompactionJob` anywhere; only `VersionEdit` _bookkeeping_ for compact pointers and level sizing), vLog GC, snapshots, a merge/k-way iterator, and the vLog write path.
- `src/sstable/table_reader.cc` does not implement blob reads: it returns the `BlobHandle` untouched and all vLog I/O happens through `BlobResolver`. Anything describing reads as `not_supported` is stale.
- Much of the working tree is uncommitted (`M`/`??` under `src/sstable/`, `src/db/`, `include/anchordb/options.h`). Check `git --no-optional-locks status` before assuming a file matches `HEAD`.

## Build

- Configure: `cmake --preset default` → Unix Makefiles, `Debug`, `build/`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`.
- Build: `cmake --build build -j`; output is the static lib `build/libanchordb.a`.
- The preset hardcodes `CMAKE_TOOLCHAIN_FILE=$env{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake`. `vcpkg.json` pulls `zstd`, `snappy`, `lz4`, `zlib`, `gperftools` into `build/vcpkg_installed/`.
- Linux: `liburing` is optional (`ANCHORDB_USE_IO_URING` defaults ON); `pkg_check_modules(LIBURING liburing)` sets `HAS_IO_URING=1`, else the build **warns** and compiles with `HAS_IO_URING=0` (synchronous I/O).
- `.clangd` reads the compilation database from `build/`, so configure before expecting clangd to resolve headers.
- x86_64 builds add `-msse4.2 -mavx2`; IPO/LTO is enabled when supported.

## Formatting / style

- Run `clang-format -i <file>` after edits. `.clang-format`: LLVM base, 4-space indent, `IndentPPDirectives: BeforeHash`, short if/loop/function/block allowed on one line.
- Headers use `#pragma once`.
- New sources need the `// Copyright 2026 Tassok Imam Wasi` + Apache-2.0 header block, matching neighboring files.

## Architecture (verified non-obvious points)

- Public API lives in `include/anchordb/*.h`; private code under `src/` is reachable via the `PRIVATE` include dir `src/`. The vLog data path is `ValueRef` (`src/db/db_format.h`) from the storage layer, then `BlobResolver` -> `VLogReaderCache` -> `RandomAccessFileReader`.
- Core extension points `Comparator`, `Compressor`, and `Filter` are **plain function-pointer structs**, not virtual classes; follow that idiom. `Env`/`Logger`/readers/writers are the exception and use virtual classes (`EnvWrapper` is the decorator base).
- Internal key encoding (memtable + SSTable): `| varint32(key_len) | user_key | timestamp (8B) | type (1B) |`. There is **no `(seq << 8) | type` 56-bit sequence trailer** — this engine versions by an explicit 8-byte timestamp plus a 1-byte `EntryType`. The varint length prefix is part of the _memtable_ key only (required because `Skiplist::seek()` compares the lookup buffer against stored entries through the same comparator). See `src/memtable/entry.h`, `src/db/db_format.h`.
- Memtable ordering (`KeyComparator` in `src/memtable/table.h`): user_key ascending, then timestamp **descending** (newest first), then `EntryType` **descending**. A seek passes `UINT64_MAX` + `EntryType::Blob` so the lookup key sorts before every stored version.
- Allocator: only `Arena` exists (`src/memory/arena.h`), a block bump allocator over global `operator new[]`; there is no `Allocator` interface or wrapper type. tcmalloc is linked process-wide by CMake.
- Memtable storage is one arena-backed single-writer/multi-reader skiplist: `src/memtable/skiplist.h` (`Skiplist<const char*, KeyComparator>`, aliased `Table` in `table.h`). Writer serialization is the caller's responsibility.
- Platform I/O split: `src/env/env.cc` + `posix.cc` (io_uring for random access, pread/pwrite fallback) on Unix, `src/port/win/env_win.cc` + `io_win.cc` (IOCP) on Windows. File handles are `*_reader`/`*_writer` abstractions under `src/file/` (`PosixRandomAccessFileReader` is the POSIX one).
- **O_DIRECT is implemented and on by default** (`ANCHORDB_DIRECT_IO_DEFAULT=ON`). Buffers go through `AlignedBuffer` (`src/util/aligned_buffer.h`), which rounds capacity up to its `alignment` (default 4096). There is no `k_o_direct_alignment` constant — the 4096 is a default argument, not a named symbol in `src/file/`.
- Three cache consumers share one idiom: `TableCache` (`src/sstable/table_cache.h`), `VLogReaderCache` (`src/db/vlog_reader_cache.h`), and `BlockCache` (`src/sstable/block_cache.h`). All take a borrowed `Cache*`, key their entries with a private `CacheKeyNamespace` id, store a raw pointer as the value, and free it through a static `delete_entry` deleter. Lookup-or-open goes through `cache_lookup_or_open<V>()` in `src/cache/cache_wrapper.h`. **Ownership is `Cache::Handle` (RAII) on every path** — `BlockCache::get` returns a pin and never hands the caller something to free, so do not reintroduce a null-cache branch or a public `destroy()`.
- `TableCache` (owns its `Cache` via `CacheWrapper`) and `VLogReaderCache` take ownership; `BlockCache` borrows and requires a non-null cache (a capacity-0 cache keeps the read path but declines retention). `TableReader::open` and `TableCache` likewise reject a null block cache, which is why `Options::block_cache` must be materialized before either is constructed.
- Trust `include/anchordb/options.h` for defaults: `block_size` = 4 KB, `data_block_size` = 32 KB (uncompressed target for framed `-Data.adb` blocks), `write_buffer_size`/`max_file_size` = 4 MB/2 MB, `table_cache_size`/`vlog_reader_cache_size`/`block_cache_size` = 8 MB each, `max_open_files` = 1000, `num_levels` = 7. Cache replacement policy and hash-map backend are selected by `CacheType`/`MapType` enums.
- `Options::block_cache_size`/`block_cache_type` are declared but nothing constructs that cache yet — the wiring belongs in the missing DB layer.

## Gotchas

- `HAS_ZSTD/HAS_SNAPPY/HAS_LZ4/HAS_ZLIB` are set by CMake `find_package`; `src/port/port_stdcxx.h` defaults any unset one to 0. Compression sources always compile, so a missing library silently disables that codec (pass-through) instead of failing the build.
- tcmalloc (`ANCHORDB_USE_TCMALLOC`, default ON) is linked `PUBLIC` so its global new/delete override reaches every consumer; if not found CMake only warns.
- `build/`, `.cache/`, and `compile_commands.json` (symlink into `build/`) are generated — never edit them.
- The SSTable read path is `filter -> summary -> index block -> BlockCache`; `-Data.adb` blocks are framed, CRC-checked by `DataBlockReader`, and cached decompressed. `BlockCache` and `DataBlockReader` (`src/sstable/data_block.*`) are new and uncommitted.
- `EntryType::Compressed` is vLog-only. It must never reach a memtable, a `-Data.adb` record, or an SSTable index slot; `TableBuilder::add` rejects it.
- `BlobHandle` values are **not** read by `TableReader`: it returns the handle (`file_id` 4B, `size` 4B, `offset` 8B; `offset`/`size` span the whole encoded vLog record) and `BlobResolver` does all vLog I/O through `VLogReaderCache`.
