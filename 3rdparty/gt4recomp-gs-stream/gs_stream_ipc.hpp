// SPDX-License-Identifier: MIT
// Shared-memory transport for the GS stream protocol (gs_stream_protocol.h; docs/GS-STREAM-PROTOCOL.md section 3).
//
// One named mapping per session ("Local\<prefix>-<producer pid>-<128-bit random hex>") holds the control block, the
// producer -> consumer command ring, the readback response area and the consumer -> producer event ring. Both rings
// are single-producer/single-consumer byte rings of 16-byte-aligned records with the discipline of
// native/mtvu_ring.hpp: a wrap filler when a record does not fit before the end, the largest record is capacity/2
// (so a filler plus the record always fit into an empty ring), callers split larger data with kRecordFlagMore, and
// positions are monotonic 64-bit counters.
//
// Wake-ups use four named auto-reset events ("<name>-data", "-space", "-response", "-event"). A waiter spins for a
// short time, then sets its waiter flag in the control block, re-checks its condition and sleeps; the other side
// signals the event only when the flag is set. Position stores and flag loads are sequentially consistent on both
// sides, so a wake-up is never lost. Every sleep also waits on the peer process handle (when one is set), so the
// death of the peer ends the wait with PeerExited instead of a hang.
//
// Threading: one producer thread and one consumer thread per ring. The Session object itself is not shared.
//
// macOS / POSIX: the mapping is a POSIX shared memory object ("/gs<last 24 hex digits of the name>m") and the four
// events are four 32-bit wake counters in a second object ("...w"). A signal increments its counter and wakes one
// waiter (os_sync_wake_by_address_any, shared); a sleeper waits while the counter still holds the value it read
// before re-checking its condition, so the waiter-flag protocol above is unchanged. A peer process "handle" is its
// pid; sleeps last at most one slice and check that the pid is still alive. The ControlBlock layout is identical.
#pragma once

#include "gs_stream_protocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace gs_stream::ipc {

using NativeHandle = void *;  // HANDLE on Windows; the pid (as an integer) of a peer process on POSIX

class IpcError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
// The peer process ended (its handle was signalled) while this side waited for it.
class PeerExited : public IpcError {
 public:
  using IpcError::IpcError;
};
// A bounded wait (handshake, shutdown) did not finish in time.
class TimedOut : public IpcError {
 public:
  using IpcError::IpcError;
};

// A record inside a ring, valid until it is released.
struct RecordView {
  const RecordHeader *header = nullptr;
  const std::uint8_t *payload = nullptr;
  std::uint64_t total_bytes = 0;  // header + padded payload: the bytes this record occupies in the ring
};

// Producer end of one SPSC byte ring living in shared memory.
class RingWriter {
 public:
  RingWriter() = default;
  RingWriter(std::uint8_t *base, std::uint64_t capacity, std::uint64_t *write_position, std::uint64_t *read_position);

  std::uint64_t capacity() const { return capacity_; }
  // Largest payload of one record; larger data must be split by the caller (kRecordFlagMore).
  std::uint64_t max_payload() const { return capacity_ / 2 - sizeof(RecordHeader); }
  std::uint64_t used_bytes() const;

  // Writes the header, the two payload parts (either may be empty) and zero padding when the record fits now.
  // Returns false when it does not fit yet. Throws IpcError when the payload exceeds max_payload().
  bool try_write(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes, const void *second,
                 std::size_t second_bytes);

  std::uint64_t records_written() const { return sequence_; }
  std::uint64_t bytes_written() const { return bytes_written_; }

 private:
  std::uint8_t *base_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t *write_ = nullptr;
  std::uint64_t *read_ = nullptr;
  std::uint64_t sequence_ = 0;
  std::uint64_t bytes_written_ = 0;
};

// Consumer end of one SPSC byte ring living in shared memory.
class RingReader {
 public:
  RingReader() = default;
  RingReader(std::uint8_t *base, std::uint64_t capacity, std::uint64_t *write_position, std::uint64_t *read_position);

  bool has_data() const;
  // Returns the next real record (wrap fillers are consumed on the way). False when the ring is empty.
  // Throws IpcError on a malformed header.
  bool peek(RecordView &view);
  // Frees the space of the record returned by the last peek.
  void release(const RecordView &view);

 private:
  std::uint8_t *base_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t *write_ = nullptr;
  std::uint64_t *read_ = nullptr;
};

struct SessionOptions {
  std::string prefix = "gt4gs";
  std::uint64_t command_capacity = kDefaultCommandCapacity;
  std::uint64_t response_capacity = kDefaultResponseCapacity;
  std::uint64_t event_capacity = kDefaultEventCapacity;
  std::uint32_t frames_in_flight = 2;
  std::string serial;  // at most 15 characters
};

enum class Wake { data, space, response, event };

// Counters a side keeps about its own waits (host microseconds).
// Optional callbacks around every wait that sleeps or spins (the game opens a perf scope for back-pressure).
struct WaitHooks {
  void (*begin)(Wake which) = nullptr;
  void (*end)(Wake which) = nullptr;
};

struct WaitTotals {
  std::uint64_t space_wait_us = 0, space_waits = 0;
  std::uint64_t frames_wait_us = 0, frames_waits = 0;
  std::uint64_t response_wait_us = 0;
  std::uint64_t data_wait_us = 0;
  // TASK-041: events signalled to a sleeping peer (one SetEvent each), indexed by Wake.
  std::uint64_t wakes_signalled[4] = {};
};

class Session {
 public:
  // Producer: creates a fresh mapping and the four events, fills the control block and sets kStateProducerReady.
  static std::unique_ptr<Session> create_producer(const SessionOptions &options);
  // Consumer: opens an existing session by name and validates magic, major version, struct sizes and the region
  // layout. Throws IpcError naming the first mismatch.
  static std::unique_ptr<Session> open_consumer(const std::string &name);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  const std::string &name() const { return name_; }
  bool is_producer() const { return producer_; }
  ControlBlock &control() { return *control_; }
  std::uint8_t *response_area() { return response_; }
  std::uint64_t response_capacity() const { return control_->response_capacity; }

  // The peer process handle used by every sleep (not owned; may be null for same-process tests).
  void set_peer(NativeHandle process) { peer_ = process; }
  // Spin time before a sleep, in microseconds (default 30).
  void set_spin_us(std::uint32_t spin_us) { spin_us_ = spin_us; }
  // TASK-041: producer wake batching. A sleeping consumer is woken after `bytes` of GIF/PRIV records since the last
  // wake-up, at once for every other record type (VSYNC, READ_FIFO, RESET, CONTROL), and before the producer waits
  // itself (ring space). 0 wakes it for every record (slice191-196 behaviour). Default kDefaultDataWakeBytes.
  static constexpr std::uint64_t kDefaultDataWakeBytes = 256u << 10;
  void set_data_wake_bytes(std::uint64_t bytes) { data_wake_bytes_ = bytes; }
  std::uint64_t data_wake_bytes() const { return data_wake_bytes_; }
  const WaitTotals &wait_totals() const { return waits_; }
  void set_wait_hooks(WaitHooks hooks) { hooks_ = hooks; }

  // ---- producer API ----
  RingWriter &commands_out();
  RingReader &events_in();
  // Posts one record, waiting for ring space as needed (back-pressure). Wakes a sleeping consumer.
  void post(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes, const void *second = nullptr,
            std::size_t second_bytes = 0);
  // Posts VSYNC, then waits while more than frames_in_flight VSYNCs are unprocessed.
  void post_vsync(const VsyncPayload &vsync);
  // Posts READ_FIFO and waits for the reply; copies qwords*16 bytes to `out`. Returns the readback status.
  std::uint32_t read_fifo(std::uint32_t qwords, void *out);
  // Waits for an event record; false on timeout. Throws PeerExited when the consumer died first.
  bool wait_event(RecordView &view, std::uint32_t timeout_ms);

  // ---- consumer API ----
  RingReader &commands_in();
  RingWriter &events_out();
  // Waits for the next command record (forever unless the peer dies or kStateFatal is set; then throws).
  void next_command(RecordView &view);
  // Releases a command record and wakes a producer waiting for space.
  void release_command(const RecordView &view);
  // Marks a VSYNC as processed (vsyncs_done) and wakes a producer waiting for frames in flight.
  void complete_vsync();
  // Publishes the reply of READ_FIFO `request_id` (bytes already in response_area()).
  void complete_readback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status);
  // Posts an event record (waits for event-ring space).
  void post_event(RecordType type, const void *payload, std::size_t bytes);

  // Raw event signalling (fixtures use it to check spurious wakes).
  void signal(Wake which);

 private:
  Session() = default;
  void map_regions();
  template <typename Ready>
  void wait_until(Ready ready, std::uint32_t *waiter, Wake which, std::uint64_t *wait_us, std::uint32_t timeout_ms,
                  const char *what);
  void wake_if_waiting(std::uint32_t *waiter, Wake which);

  bool producer_ = false;
  std::string name_;
#ifdef _WIN32
  NativeHandle mapping_ = nullptr;
#endif
  std::uint8_t *view_ = nullptr;
  ControlBlock *control_ = nullptr;
  std::uint8_t *response_ = nullptr;
#ifdef _WIN32
  NativeHandle events_[4] = {};
#else
  std::uint64_t mapped_bytes_ = 0;
  std::uint32_t *wake_words_ = nullptr;  // the four wake counters (Wake order), in their own shared object
#endif
  NativeHandle peer_ = nullptr;
  std::uint32_t spin_us_ = 30;
  std::uint64_t data_wake_bytes_ = kDefaultDataWakeBytes;
  std::uint64_t bytes_since_data_wake_ = 0;  // producer: record bytes posted since the last data wake-up check
  RingWriter writer_;
  RingReader reader_;
  WaitTotals waits_;
  WaitHooks hooks_;
};

// A child process in a Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: closing the job (destructor) ends it.
// POSIX: a posix_spawn child that the destructor kills (SIGKILL) and reaps when it still runs; children still running
// at a normal process exit are killed by an atexit handler. Nothing ends them when this process is itself killed.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess &) = delete;
  ChildProcess &operator=(const ChildProcess &) = delete;
  ChildProcess(ChildProcess &&other) noexcept;
  ChildProcess &operator=(ChildProcess &&other) noexcept;

  // Starts `executable` with `arguments` (one command-line string, appended after the quoted executable path),
  // suspended, assigns it to a new kill-on-close job and resumes it. Standard handles are inherited.
  static ChildProcess spawn(const std::wstring &executable, const std::wstring &arguments);

  NativeHandle process() const { return process_; }
  std::uint32_t pid() const { return pid_; }
  bool running() const;
  // Waits up to timeout_ms for the process to exit; true when it exited. exit_code receives its code.
  bool wait_exit(std::uint32_t timeout_ms, std::uint32_t *exit_code = nullptr) const;
  // Kills the process (TerminateJobObject) and waits for it.
  void kill();

 private:
  NativeHandle process_ = nullptr;
  NativeHandle job_ = nullptr;
  std::uint32_t pid_ = 0;
};

// Opens the process `pid` for SYNCHRONIZE (consumer side: the producer is the peer). Null on failure.
NativeHandle open_process_for_wait(std::uint32_t pid);
void close_handle(NativeHandle handle);
std::uint32_t current_process_id();
std::wstring widen(const std::string &text);
#ifndef _WIN32
// UTF-32 (wchar_t on POSIX) to UTF-8, the inverse of widen.
std::string narrow(const std::wstring &text);
#endif

}  // namespace gs_stream::ipc
