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
#include "anchordb/status.h"
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anchordb/anchordb_namespace.h"

namespace ANCHORDB_NAMESPACE {

class FileLock;
class Logger;
class AlignedBuffer;
class ReadBatcher;
class RandomAccessFileReader;
class SequentialFileReader;
class WritableFileWriter;
class Logger;
struct BatchReadOp;

// Operating-system abstraction: every filesystem, locking, scheduling and
// timing call the engine makes goes through an Env, which makes the engine
// portable and testable (a test can install its own Env).
//
// Obtain the platform default with default_env().  If nothing has been
// installed it is created on first use and owned by the library.
class ANCHORDB_EXPORT Env {
  public:
    Env() = default;

    Env(const Env &) = delete;
    Env &operator=(const Env &) = delete;

    virtual ~Env();

    // Returns the process-wide Env, creating the platform default on first
    // call.  Never null.
    static Env *default_env();

    // Overrides the environment returned by default_env().  Pass nullptr to
    // clear the override and fall back to the platform default.  The caller
    // retains ownership and must keep `env` alive while it is installed.
    static void set_default_env(Env *env);

    // Opens `fname` for sequential reading.  `buffer_size` of 0 selects a
    // sensible default.
    virtual Status new_sequential_file_reader(const std::string &fname, SequentialFileReader **result,
                                              size_t buffer_size = 0) = 0;
    // Opens `fname` for random (positional) reading.
    virtual Status new_random_access_file_reader(const std::string &fname, RandomAccessFileReader **result) = 0;
    // Creates (or truncates) `fname` for buffered writing.
    virtual Status new_writable_file_writer(const std::string &fname, WritableFileWriter **result,
                                            size_t buffer_size = 0) = 0;
    // Opens `fname` for appending, preserving any existing contents.
    virtual Status new_appendable_file_writer(const std::string &fname, WritableFileWriter **result,
                                              size_t buffer_size = 0) = 0;

    // Returns whether `fname` exists.
    virtual bool file_exists(const std::string &fname) = 0;
    // Appends the names of the entries in directory `dir` to `*children`.
    virtual Status get_children(const std::string &dir, std::vector<std::string> *children) = 0;
    // Deletes the file `fname`.
    virtual Status remove_file(const std::string &fname) = 0;
    // Creates directory `dirname`.
    virtual Status create_dir(const std::string &dirname) = 0;
    // Removes the empty directory `dirname`.
    virtual Status remove_dir(const std::string &dirname) = 0;
    // Stores the size in bytes of `fname` in `*size`.
    virtual Status get_file_size(const std::string &fname, uint64_t *size) = 0;
    // Renames `src` to `target`, replacing `target` if it exists.
    virtual Status rename_file(const std::string &src, const std::string &target) = 0;
    // Truncates `fname` to exactly `size` bytes, making the change durable
    // before returning. The default reports NotSupported so an Env that cannot
    // truncate still compiles and lets the caller take a fallback path.
    virtual Status truncate_file(const std::string &fname, uint64_t size) {
        (void)fname;
        (void)size;
        return Status::not_supported("truncate_file");
    }
    // Flushes the directory's metadata (for example after a rename) so the
    // change survives a crash.  Environments without directory fsync keep the
    // default no-op implementation.
    virtual Status sync_dir(const std::string &dirname) {
        (void)dirname;
        return Status::ok();
    }

    // Acquires an exclusive lock on `fname`, storing the lock token in `*lock`.
    // The caller owns the token and releases it with unlock_file().
    virtual Status lock_file(const std::string &fname, FileLock **lock) = 0;
    // Releases a lock previously obtained from lock_file().
    virtual Status unlock_file(FileLock *lock) = 0;

    // Schedules `function(arg)` to run on a background thread.  Returns
    // immediately; the work is best-effort and may be dropped if the Env is
    // shutting down.
    virtual void schedule(void (*function)(void *arg), void *arg) = 0;
    // Returns a directory suitable for temporary test files.
    virtual Status get_test_dir(std::string *path) = 0;

    // Opens `fname` as an append-only logger, storing it in `*result`.
    virtual Status new_logger(const std::string &fname, Logger **result) = 0;

    // Creates a batcher that runs many random reads concurrently, storing it
    // in `*result`.  The caller owns the batcher; the environment keeps
    // whatever ring or resources back it.  The default reports NotSupported so
    // an Env without batching support still compiles and lets the caller fall
    // back to per-file reads.
    virtual Status new_read_batcher(ReadBatcher **result) {
        (void)result;
        return Status::not_supported("new_read_batcher");
    }

    // Returns the batcher shared by all readers on the calling thread, creating
    // it on first use, or null when this Env cannot batch.  Readers resolve
    // their batcher per call through this, so a reader is never bound to the
    // batcher of the thread that opened it.  The Env owns the batcher for the
    // lifetime of the thread.
    virtual ReadBatcher *read_batcher_for_thread() { return nullptr; }

    // Returns the current time in microseconds since the Unix epoch.
    virtual uint64_t now_micros() = 0;
    // Sleeps for approximately `micros` microseconds.
    virtual void sleep_for_micros(int micros) = 0;
};

// Reads a file as a forward-only byte stream.
class ANCHORDB_EXPORT SequentialFileReader {
  public:
    SequentialFileReader() = default;
    virtual ~SequentialFileReader();

    SequentialFileReader(const SequentialFileReader &) = delete;
    SequentialFileReader &operator=(const SequentialFileReader &) = delete;

    // Reads up to `n` bytes into `scratch` and points `*result` at them; the
    // view is valid until the next call.  `*result` may be shorter than `n` at
    // end of file.
    virtual Status read(size_t n, std::string_view *result, char *scratch) = 0;
    // Advances the read position by `n` bytes without copying.
    virtual Status skip(uint64_t n) = 0;
};

// Reads a file at arbitrary offsets.
class ANCHORDB_EXPORT RandomAccessFileReader {
  public:
    RandomAccessFileReader() = default;

    RandomAccessFileReader(const RandomAccessFileReader &) = delete;
    RandomAccessFileReader &operator=(const RandomAccessFileReader &) = delete;

    virtual ~RandomAccessFileReader();

    // Reads up to `n` bytes at absolute `offset` into `scratch` and points
    // `*result` at them; the view is valid until the next call.  `*result` may
    // be shorter than `n` at end of file.
    virtual Status read(uint64_t offset, size_t n, std::string_view *result, char *scratch) = 0;

    // Queues a read of `n` bytes at `offset` into `scratch` on the batcher
    // returned by batcher(), instead of doing it now.  The batcher writes the
    // outcome into `*status` and `*bytes_read` when the caller later calls
    // execute() on it, so those pointers and `scratch` must stay valid until
    // then.  Returns NotSupported when this reader cannot batch, and the caller
    // must then read() synchronously.
    virtual Status queue_read(uint64_t offset, size_t n, char *scratch, Status *status, size_t *bytes_read,
                              bool *done = nullptr) {
        (void)offset;
        (void)n;
        (void)scratch;
        (void)status;
        (void)bytes_read;
        (void)done;
        return Status::not_supported("queue_read");
    }

    // The batcher queue_read() submits to, which the caller must execute() to
    // run the queued reads.  Null when this reader cannot batch.
    virtual ReadBatcher *batcher() const { return nullptr; }

    // Reads several regions of this file in one batch, overlapping their I/O
    // instead of paying each round trip serially.
    //
    // `ops[i]` names an offset, a length and a destination; the reader fills in
    // each op's `status` and `bytes_read` and leaves the bytes in `ops[i].dst`.
    // The reader supplies its own descriptor, so every op addresses this file.
    // `dst`, `status` and `bytes_read` must stay valid for the duration of the
    // call.  A short `bytes_read` means end of file and is not an error.
    //
    // Returns the first per-op error; as with ReadBatcher::execute(), one
    // failing op does not stop the others.  Environments without batching
    // report NotSupported so the caller can fall back to read().
    virtual Status read_batch(std::span<BatchReadOp> ops) = 0;
};

// One pending read handed to a ReadBatcher, describing a pread of `size` bytes
// at absolute `offset` into `dst`.
//
// The descriptor is not part of the operation: ReadBatcher::add() names the
// file it reads from, because a batch belongs to one file at a time.
//
// The caller owns `dst` and keeps it valid until the batcher completes the
// operation.  The batcher owns the results: after the operation completes it
// writes the outcome into `*status` and the bytes actually transferred into
// `bytes_read`.  Either pointer may be null if the caller does not want that
// result.  A `bytes_read` smaller than `size` means end of file; that is not
// an error.
//
// With direct I/O `dst`, `offset` and `size` must be aligned to the device
// block size, or the read is serviced through an aligned bounce buffer.
struct BatchReadOp {
    uint64_t offset = 0;
    size_t size = 0;
    char *dst = nullptr;
    Status *status = nullptr;
    size_t *bytes_read = nullptr;
    // Optional completion flag: set true once the operation has finished,
    // whether it succeeded or failed.  Lets a caller poll a specific operation.
    bool *done = nullptr;
};

// Executes many independent random reads as one batch so their latency
// overlaps instead of being paid serially.
//
// A batcher is a staging area with a single in-flight generation:
//
//   add(...)           record operations (only while nothing is in flight)
//   submit()           start them; returns without waiting
//   poll()             reap whatever has finished without blocking
//   wait()             reap until everything is done, then clear the queue
//   execute()          submit() + wait(), for callers that cannot overlap work
//
// Calls must be serialized by the caller. The batch's buffers and descriptors
// must stay valid from add() until wait()/execute() returns.  Every operation
// reports its own outcome through the BatchReadOp it was added with, so one
// failure does not stop the others.
class ANCHORDB_EXPORT ReadBatcher {
  public:
    ReadBatcher() = default;

    ReadBatcher(const ReadBatcher &) = delete;
    ReadBatcher &operator=(const ReadBatcher &) = delete;

    virtual ~ReadBatcher();

    // Queues `op`, reading from the caller's already-open descriptor `fd`.  `fd`
    // must stay open until the batch completes, and so must the operation's
    // `dst`, `status`, `bytes_read` and `done` storage.  Returns false when a
    // previous generation is still in flight (call wait() first); the caller
    // should then fall back to a synchronous read.
    virtual bool add(int fd, const BatchReadOp &op) = 0;

    // Starts every queued operation without waiting for completions.  On
    // success the operations are in flight; poll()/wait() must finish them
    // before the queue is reused.  A batcher without asynchronous I/O performs
    // the reads here, so this call may block in that configuration.
    virtual Status submit() = 0;

    // Reaps any completions available right now without blocking.  Returns true
    // when every queued operation has completed (including an empty queue).
    virtual bool poll() = 0;

    // Blocks until every queued operation has completed, then clears the queue
    // and returns the first error encountered.
    virtual Status wait() = 0;

    // submit() then wait(); clears the queue.  Returns the first error.
    virtual Status execute() = 0;
};

// Writes a file as an append-only byte stream.
class ANCHORDB_EXPORT WritableFileWriter {
  public:
    WritableFileWriter() = default;

    WritableFileWriter(const WritableFileWriter &) = delete;
    WritableFileWriter &operator=(const WritableFileWriter &) = delete;

    virtual ~WritableFileWriter();

    // Appends `data`; the bytes may be buffered until flush()/sync()/close().
    virtual Status append(std::string_view data) = 0;
    // Flushes buffered data and closes the file.
    virtual Status close() = 0;
    // Flushes buffered data to the OS (not necessarily durable).
    virtual Status flush() = 0;
    // Flushes and fsyncs, making prior appends durable.
    virtual Status sync() = 0;
};

// Append-only text logger.
class ANCHORDB_EXPORT Logger {
  public:
    Logger() = default;

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    virtual ~Logger();

    // Writes one formatted entry (printf-style, with `ap`) to the log file.
    virtual void Logv(const char *format, std::va_list ap) = 0;
};

// Token for an acquired file lock; the type is opaque to callers and is only
// used to pass the lock back to Env::unlock_file().
class ANCHORDB_EXPORT FileLock {
  public:
    FileLock() = default;

    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    virtual ~FileLock();
};

// Env decorator that forwards every call to a target Env.  Subclass it and
// override only the methods you need, e.g. to intercept file creation in a
// test; all other calls keep their default behavior.
class ANCHORDB_EXPORT EnvWrapper : public Env {
  public:
    // Wraps `t`; all calls are delegated to it.  `t` must outlive the wrapper.
    explicit EnvWrapper(Env *t) : target_(t) {}
    virtual ~EnvWrapper();

    // Returns the wrapped Env.
    Env *target() const { return target_; }

    // The rest of the class forwards every Env method to target().
    Status new_sequential_file_reader(const std::string &f, SequentialFileReader **r, size_t buffer_size = 0) override {
        return target_->new_sequential_file_reader(f, r, buffer_size);
    }
    Status new_random_access_file_reader(const std::string &f, RandomAccessFileReader **r) override {
        return target_->new_random_access_file_reader(f, r);
    }
    Status new_writable_file_writer(const std::string &f, WritableFileWriter **r, size_t buffer_size = 0) override {
        return target_->new_writable_file_writer(f, r, buffer_size);
    }
    Status new_appendable_file_writer(const std::string &f, WritableFileWriter **r, size_t buffer_size = 0) override {
        return target_->new_appendable_file_writer(f, r, buffer_size);
    }
    bool file_exists(const std::string &f) override { return target_->file_exists(f); }
    Status get_children(const std::string &dir, std::vector<std::string> *r) override {
        return target_->get_children(dir, r);
    }
    Status remove_file(const std::string &f) override { return target_->remove_file(f); }
    Status create_dir(const std::string &d) override { return target_->create_dir(d); }
    Status remove_dir(const std::string &d) override { return target_->remove_dir(d); }
    Status get_file_size(const std::string &f, uint64_t *s) override { return target_->get_file_size(f, s); }
    Status rename_file(const std::string &s, const std::string &t) override { return target_->rename_file(s, t); }
    Status truncate_file(const std::string &f, uint64_t size) override { return target_->truncate_file(f, size); }
    Status sync_dir(const std::string &d) override { return target_->sync_dir(d); }
    Status lock_file(const std::string &f, FileLock **l) override { return target_->lock_file(f, l); }
    Status unlock_file(FileLock *l) override { return target_->unlock_file(l); }
    void schedule(void (*f)(void *), void *a) override { return target_->schedule(f, a); }
    Status get_test_dir(std::string *path) override { return target_->get_test_dir(path); }
    Status new_logger(const std::string &fname, Logger **result) override { return target_->new_logger(fname, result); }
    Status new_read_batcher(ReadBatcher **r) override { return target_->new_read_batcher(r); }
    ReadBatcher *read_batcher_for_thread() override { return target_->read_batcher_for_thread(); }
    uint64_t now_micros() override { return target_->now_micros(); }
    void sleep_for_micros(int micros) override { target_->sleep_for_micros(micros); }

  private:
    Env *target_;
};

} // namespace ANCHORDB_NAMESPACE
