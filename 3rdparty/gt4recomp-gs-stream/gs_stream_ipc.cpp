// SPDX-License-Identifier: MIT
// Shared-memory transport for the GS stream protocol; see gs_stream_ipc.hpp.
#include "gs_stream_ipc.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <immintrin.h>
#else
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <vector>

extern char **environ;
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>

namespace gs_stream::ipc {
namespace {

constexpr std::uint64_t kPageBytes = 4096;
constexpr std::uint32_t kSleepSliceMs = 100;  // a sleeping side re-checks the fatal flag at least this often
constexpr std::uint32_t kWaitForever = 0xFFFFFFFFu;  // kWaitForever
#ifdef _WIN32
const wchar_t *const kEventSuffix[4] = {L"-data", L"-space", L"-response", L"-event"};
#endif

void cpu_relax() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#elif defined(__aarch64__)
  __builtin_arm_yield();
#endif
}

std::uint64_t load(const std::uint64_t &value, std::memory_order order = std::memory_order_seq_cst) {
  // atomic_ref<const T> is C++26 (libc++ rejects it); the load does not write.
  return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t &>(value)).load(order);
}
void store(std::uint64_t &value, std::uint64_t next, std::memory_order order = std::memory_order_seq_cst) {
  std::atomic_ref<std::uint64_t>(value).store(next, order);
}
std::uint32_t load32(const std::uint32_t &value) {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t &>(value)).load(std::memory_order_seq_cst);
}
void store32(std::uint32_t &value, std::uint32_t next) { std::atomic_ref<std::uint32_t>(value).store(next, std::memory_order_seq_cst); }

std::uint64_t now_us() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t unit) { return (value + unit - 1) / unit * unit; }

#ifdef _WIN32
std::string last_error_text(const char *what) {
  return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}
#else
std::string last_error_text(const char *what) {
  return std::string(what) + " failed (" + std::strerror(errno) + ")";
}

// POSIX shared memory names are limited to 31 characters on macOS: "/gs" + the last 24 hex digits of the session
// name (its random part) + "m" (mapping) or "w" (wake counters).
constexpr std::size_t kShmKeyDigits = 24;
std::string shm_name(const std::string &session_name, char kind) {
  if (session_name.size() < kShmKeyDigits) throw IpcError("GS stream session name too short: " + session_name);
  const std::string key = session_name.substr(session_name.size() - kShmKeyDigits);
  if (key.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw IpcError("GS stream session name does not end in its random key: " + session_name);
  return "/gs" + key + kind;
}

pid_t peer_pid(NativeHandle handle) { return static_cast<pid_t>(reinterpret_cast<std::uintptr_t>(handle)); }

// True while `pid` runs. A child that exited but was not reaped yet counts as exited (WNOWAIT keeps its status
// for ChildProcess::wait_exit); for other processes kill(pid, 0) answers.
bool process_alive(pid_t pid) {
  siginfo_t info{};
  if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) return info.si_pid != pid;
  return kill(pid, 0) == 0 || errno == EPERM;
}

void *map_shared(int descriptor, std::uint64_t bytes) {
  void *view = mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  return view == MAP_FAILED ? nullptr : view;
}

// A new, zero-filled shared object of `bytes`; fails when the name already exists.
void *create_shared(const std::string &name, std::uint64_t bytes) {
  const int descriptor = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    if (errno == EEXIST) throw IpcError("GS stream mapping name already exists");
    throw IpcError(last_error_text("shm_open"));
  }
  if (ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
    const std::string text = last_error_text("ftruncate");
    close(descriptor);
    shm_unlink(name.c_str());
    throw IpcError(text);
  }
  void *view = map_shared(descriptor, bytes);
  const std::string text = last_error_text("mmap");
  close(descriptor);
  if (!view) {
    shm_unlink(name.c_str());
    throw IpcError(text);
  }
  return view;
}

// Opens an existing shared object; `bytes` receives its size.
void *open_shared(const std::string &name, const std::string &session_name, std::uint64_t &bytes) {
  const int descriptor = shm_open(name.c_str(), O_RDWR, 0);
  if (descriptor < 0) throw IpcError(last_error_text("shm_open") + " for " + session_name);
  struct stat status {};
  if (fstat(descriptor, &status) != 0) {
    const std::string text = last_error_text("fstat");
    close(descriptor);
    throw IpcError(text);
  }
  bytes = static_cast<std::uint64_t>(status.st_size);
  void *view = bytes ? map_shared(descriptor, bytes) : nullptr;
  const std::string text = last_error_text("mmap");
  close(descriptor);
  if (!view) throw IpcError(text);
  return view;
}

}  // namespace

std::string narrow(const std::wstring &text) {
  std::string out;
  for (const wchar_t wide : text) {
    const auto code = static_cast<std::uint32_t>(wide);
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }
  return out;
}

namespace {

// Splits a Windows-style command line tail: arguments separated by spaces, double quotes group (and are removed).
std::vector<std::string> split_arguments(const std::string &line) {
  std::vector<std::string> arguments;
  std::string current;
  bool quoted = false, pending = false;
  for (const char character : line) {
    if (character == '"') {
      quoted = !quoted;
      pending = true;
    } else if (character == ' ' && !quoted) {
      if (pending) arguments.push_back(current);
      current.clear();
      pending = false;
    } else {
      current.push_back(character);
      pending = true;
    }
  }
  if (pending) arguments.push_back(current);
  return arguments;
}
#endif

void check_capacity(std::uint64_t capacity, const char *what) {
  if (capacity < kPageBytes || capacity % kPageBytes != 0 || capacity > (1ull << 32))
    throw IpcError(std::string(what) + " capacity must be a multiple of 4096 between 4 KiB and 4 GiB");
}

std::string random_hex128() {
  std::random_device device;  // RtlGenRandom-backed in the Microsoft STL
  std::string text;
  static const char kDigits[] = "0123456789abcdef";
  for (int word = 0; word < 4; ++word) {
    const std::uint32_t value = device();
    for (int nibble = 7; nibble >= 0; --nibble) text.push_back(kDigits[(value >> (nibble * 4)) & 15u]);
  }
  return text;
}

}  // namespace

#ifndef _WIN32
std::wstring widen(const std::string &text) {
  // UTF-8 to UTF-32 (wchar_t is 32-bit on POSIX).
  std::wstring wide;
  for (std::size_t index = 0; index < text.size();) {
    const auto lead = static_cast<unsigned char>(text[index]);
    const int extra = lead < 0x80 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
    std::uint32_t code = extra == 0 ? lead : extra == 1 ? (lead & 0x1Fu) : extra == 2 ? (lead & 0x0Fu) : (lead & 0x07u);
    for (int part = 1; part <= extra && index + part < text.size(); ++part)
      code = (code << 6) | (static_cast<unsigned char>(text[index + part]) & 0x3Fu);
    wide.push_back(static_cast<wchar_t>(code));
    index += static_cast<std::size_t>(extra) + 1;
  }
  return wide;
}

std::uint32_t current_process_id() { return static_cast<std::uint32_t>(getpid()); }

// Peer handles are pids and own nothing.
void close_handle(NativeHandle) {}

NativeHandle open_process_for_wait(std::uint32_t pid) {
  if (pid == 0 || (kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM)) return nullptr;
  return reinterpret_cast<NativeHandle>(static_cast<std::uintptr_t>(pid));
}
#else
std::wstring widen(const std::string &text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
  return wide;
}

std::uint32_t current_process_id() { return GetCurrentProcessId(); }

void close_handle(NativeHandle handle) {
  if (handle) CloseHandle(handle);
}

NativeHandle open_process_for_wait(std::uint32_t pid) {
  return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------------------------------------------

RingWriter::RingWriter(std::uint8_t *base, std::uint64_t capacity, std::uint64_t *write_position,
                       std::uint64_t *read_position)
    : base_(base), capacity_(capacity), write_(write_position), read_(read_position) {}

std::uint64_t RingWriter::used_bytes() const { return load(*write_, std::memory_order_relaxed) - load(*read_, std::memory_order_acquire); }

bool RingWriter::try_write(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes,
                           const void *second, std::size_t second_bytes) {
  const std::uint64_t payload = static_cast<std::uint64_t>(first_bytes) + second_bytes;
  if (payload > max_payload()) throw IpcError("GS stream record larger than half the ring");
  const std::uint64_t total = record_size(payload);
  const std::uint64_t head = load(*write_, std::memory_order_relaxed);
  const std::uint64_t tail = load(*read_, std::memory_order_acquire);
  const std::uint64_t offset = head % capacity_;
  const std::uint64_t filler = offset + total > capacity_ ? capacity_ - offset : 0;
  if (capacity_ - (head - tail) < filler + total) return false;

  std::uint64_t at = offset;
  if (filler != 0) {
    RecordHeader wrap{};
    wrap.type = static_cast<std::uint16_t>(RecordType::wrap);
    wrap.payload_bytes = static_cast<std::uint32_t>(filler - sizeof(RecordHeader));
    std::memcpy(base_ + offset, &wrap, sizeof wrap);
    at = 0;
  }
  RecordHeader header{};
  header.type = static_cast<std::uint16_t>(type);
  header.flags = flags;
  header.payload_bytes = static_cast<std::uint32_t>(payload);
  header.sequence = ++sequence_;
  std::uint8_t *out = base_ + at;
  std::memcpy(out, &header, sizeof header);
  out += sizeof header;
  if (first_bytes) std::memcpy(out, first, first_bytes);
  if (second_bytes) std::memcpy(out + first_bytes, second, second_bytes);
  const std::uint64_t padding = align_record(payload) - payload;
  if (padding) std::memset(out + payload, 0, static_cast<std::size_t>(padding));
  bytes_written_ += total;
  store(*write_, head + filler + total);  // seq_cst: ordered before the waiter-flag load of the caller
  return true;
}

RingReader::RingReader(std::uint8_t *base, std::uint64_t capacity, std::uint64_t *write_position,
                       std::uint64_t *read_position)
    : base_(base), capacity_(capacity), write_(write_position), read_(read_position) {}

bool RingReader::has_data() const { return load(*write_) != load(*read_, std::memory_order_relaxed); }

bool RingReader::peek(RecordView &view) {
  std::uint64_t tail = load(*read_, std::memory_order_relaxed);
  const std::uint64_t head = load(*write_, std::memory_order_acquire);
  while (tail != head) {
    const std::uint64_t offset = tail % capacity_;
    const auto *header = reinterpret_cast<const RecordHeader *>(base_ + offset);
    const std::uint64_t total = record_size(header->payload_bytes);
    if (header->type == static_cast<std::uint16_t>(RecordType::wrap)) {
      if (total != capacity_ - offset || total > head - tail) throw IpcError("GS stream ring: malformed wrap filler");
      tail += total;
      store(*read_, tail);
      continue;
    }
    if (offset + total > capacity_ || total > head - tail || header->sequence == 0)
      throw IpcError("GS stream ring: malformed record header");
    view.header = header;
    view.payload = base_ + offset + sizeof(RecordHeader);
    view.total_bytes = total;
    return true;
  }
  return false;
}

void RingReader::release(const RecordView &view) {
  store(*read_, load(*read_, std::memory_order_relaxed) + view.total_bytes);
}

// ---------------------------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------------------------

std::unique_ptr<Session> Session::create_producer(const SessionOptions &options) {
  check_capacity(options.command_capacity, "command ring");
  check_capacity(options.event_capacity, "event ring");
  if (options.response_capacity < 16 || options.response_capacity % 16 != 0)
    throw IpcError("response capacity must be a positive multiple of 16");
  if (options.frames_in_flight < 1 || options.frames_in_flight > 3) throw IpcError("frames in flight must be 1, 2 or 3");
  if (options.serial.size() > 15) throw IpcError("serial longer than 15 characters");
  if (options.prefix.empty() || options.prefix.find_first_of("\\/") != std::string::npos)
    throw IpcError("session prefix must be a plain name");

  std::unique_ptr<Session> session(new Session);
  session->producer_ = true;
#ifdef _WIN32
  session->name_ = "Local\\" + options.prefix + "-" + std::to_string(GetCurrentProcessId()) + "-" + random_hex128();
#else
  session->name_ = options.prefix + "-" + std::to_string(current_process_id()) + "-" + random_hex128();
#endif
  const std::uint64_t command_offset = kControlBlockBytes;
  const std::uint64_t response_offset = command_offset + options.command_capacity;
  const std::uint64_t event_offset = response_offset + round_up(options.response_capacity, kPageBytes);
  const std::uint64_t mapping_bytes = event_offset + options.event_capacity;

#ifndef _WIN32
  session->view_ = static_cast<std::uint8_t *>(create_shared(shm_name(session->name_, 'm'), mapping_bytes));
  session->mapped_bytes_ = mapping_bytes;
  session->wake_words_ = static_cast<std::uint32_t *>(create_shared(shm_name(session->name_, 'w'), kPageBytes));
#else
  const std::wstring wide = widen(session->name_);
  SetLastError(0);
  session->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         static_cast<DWORD>(mapping_bytes >> 32), static_cast<DWORD>(mapping_bytes),
                                         wide.c_str());
  if (!session->mapping_) throw IpcError(last_error_text("CreateFileMappingW"));
  if (GetLastError() == ERROR_ALREADY_EXISTS) throw IpcError("GS stream mapping name already exists");
  session->view_ = static_cast<std::uint8_t *>(MapViewOfFile(session->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!session->view_) throw IpcError(last_error_text("MapViewOfFile"));
  for (int index = 0; index < 4; ++index) {
    const std::wstring event_name = wide + kEventSuffix[index];
    SetLastError(0);
    session->events_[index] = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    if (!session->events_[index]) throw IpcError(last_error_text("CreateEventW"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) throw IpcError("GS stream event name already exists");
  }
#endif

  ControlBlock &control = *reinterpret_cast<ControlBlock *>(session->view_);  // the mapping starts zeroed
  control.magic = kMagic;
  control.version_major = kVersionMajor;
  control.version_minor = kVersionMinor;
  control.control_block_bytes = kControlBlockBytes;
  control.record_header_bytes = sizeof(RecordHeader);
  control.producer_pid = current_process_id();
  control.frames_in_flight_limit = options.frames_in_flight;
  std::memcpy(control.serial, options.serial.data(), options.serial.size());
  control.mapping_bytes = mapping_bytes;
  control.command_offset = command_offset;
  control.command_capacity = options.command_capacity;
  control.response_offset = response_offset;
  control.response_capacity = options.response_capacity;
  control.event_offset = event_offset;
  control.event_capacity = options.event_capacity;
  session->map_regions();
  store32(control.state, kStateProducerReady);
  return session;
}

std::unique_ptr<Session> Session::open_consumer(const std::string &name) {
  std::unique_ptr<Session> session(new Session);
  session->name_ = name;
#ifndef _WIN32
  std::uint64_t region_bytes = 0;
  session->view_ = static_cast<std::uint8_t *>(open_shared(shm_name(name, 'm'), name, region_bytes));
  session->mapped_bytes_ = region_bytes;
  if (region_bytes < kControlBlockBytes) throw IpcError("GS stream mapping smaller than the control block");
#else
  const std::wstring wide = widen(name);
  session->mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide.c_str());
  if (!session->mapping_) throw IpcError(last_error_text("OpenFileMappingW") + " for " + name);
  session->view_ = static_cast<std::uint8_t *>(MapViewOfFile(session->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!session->view_) throw IpcError(last_error_text("MapViewOfFile"));
  MEMORY_BASIC_INFORMATION region{};
  if (!VirtualQuery(session->view_, &region, sizeof region)) throw IpcError(last_error_text("VirtualQuery"));
  if (region.RegionSize < kControlBlockBytes) throw IpcError("GS stream mapping smaller than the control block");
  const std::uint64_t region_bytes = region.RegionSize;
#endif

  const ControlBlock &control = *reinterpret_cast<const ControlBlock *>(session->view_);
  if (control.magic != kMagic) throw IpcError("GS stream magic mismatch");
  if (control.version_major != kVersionMajor)
    throw IpcError("GS stream protocol major version " + std::to_string(control.version_major) + " is not " +
                   std::to_string(kVersionMajor));
  if (control.control_block_bytes != kControlBlockBytes || control.record_header_bytes != sizeof(RecordHeader))
    throw IpcError("GS stream struct sizes differ");
  if (!(load32(control.state) & kStateProducerReady)) throw IpcError("GS stream producer is not ready");
  const auto inside = [&](std::uint64_t offset, std::uint64_t bytes) {
    return offset >= kControlBlockBytes && offset % kPageBytes == 0 && bytes <= control.mapping_bytes &&
           offset <= control.mapping_bytes - bytes;
  };
  if (control.mapping_bytes > region_bytes || !inside(control.command_offset, control.command_capacity) ||
      !inside(control.response_offset, control.response_capacity) || !inside(control.event_offset, control.event_capacity) ||
      control.command_capacity % kPageBytes != 0 || control.event_capacity % kPageBytes != 0 ||
      control.response_capacity % 16 != 0)
    throw IpcError("GS stream region layout is invalid");
  if (control.frames_in_flight_limit < 1 || control.frames_in_flight_limit > 3)
    throw IpcError("GS stream frames-in-flight limit out of range");
#ifndef _WIN32
  std::uint64_t wake_bytes = 0;
  session->wake_words_ = static_cast<std::uint32_t *>(open_shared(shm_name(name, 'w'), name, wake_bytes));
  if (wake_bytes < 4 * sizeof(std::uint32_t)) throw IpcError("GS stream wake counters too small");
#else
  for (int index = 0; index < 4; ++index) {
    const std::wstring event_name = wide + kEventSuffix[index];
    session->events_[index] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, event_name.c_str());
    if (!session->events_[index]) throw IpcError(last_error_text("OpenEventW"));
  }
#endif
  session->map_regions();
  return session;
}

void Session::map_regions() {
  control_ = reinterpret_cast<ControlBlock *>(view_);
  ControlBlock &c = *control_;
  response_ = view_ + c.response_offset;
  if (producer_) {
    writer_ = RingWriter(view_ + c.command_offset, c.command_capacity, &c.command_write, &c.command_read);
    reader_ = RingReader(view_ + c.event_offset, c.event_capacity, &c.event_write, &c.event_read);
  } else {
    reader_ = RingReader(view_ + c.command_offset, c.command_capacity, &c.command_write, &c.command_read);
    writer_ = RingWriter(view_ + c.event_offset, c.event_capacity, &c.event_write, &c.event_read);
  }
}

Session::~Session() {
#ifndef _WIN32
  // The producer created both objects and removes their names; an open consumer keeps its mapping until it unmaps.
  if (view_) munmap(view_, static_cast<std::size_t>(mapped_bytes_));
  if (wake_words_) munmap(wake_words_, static_cast<std::size_t>(kPageBytes));
  if (producer_ && !name_.empty()) {
    shm_unlink(shm_name(name_, 'm').c_str());
    shm_unlink(shm_name(name_, 'w').c_str());
  }
#else
  for (NativeHandle &event : events_) close_handle(event);
  if (view_) UnmapViewOfFile(view_);
  close_handle(mapping_);
#endif
}

RingWriter &Session::commands_out() {
  if (!producer_) throw IpcError("commands_out on the consumer side");
  return writer_;
}
RingReader &Session::events_in() {
  if (!producer_) throw IpcError("events_in on the consumer side");
  return reader_;
}
RingReader &Session::commands_in() {
  if (producer_) throw IpcError("commands_in on the producer side");
  return reader_;
}
RingWriter &Session::events_out() {
  if (producer_) throw IpcError("events_out on the producer side");
  return writer_;
}

void Session::signal(Wake which) {
#ifndef _WIN32
  std::uint32_t &word = wake_words_[static_cast<int>(which)];
  std::atomic_ref<std::uint32_t>(word).fetch_add(1, std::memory_order_seq_cst);
  os_sync_wake_by_address_any(&word, sizeof word, OS_SYNC_WAKE_BY_ADDRESS_SHARED);  // ENOENT: nobody sleeps yet
#else
  SetEvent(events_[static_cast<int>(which)]);
#endif
}

void Session::wake_if_waiting(std::uint32_t *waiter, Wake which) {
  if (load32(*waiter)) {
    ++waits_.wakes_signalled[static_cast<int>(which)];
    signal(which);
  }
}

template <typename Ready>
void Session::wait_until(Ready ready, std::uint32_t *waiter, Wake which, std::uint64_t *wait_us, std::uint32_t timeout_ms,
                         const char *what) {
  if (ready()) return;
  const std::uint64_t start = now_us();
  struct HookScope {
    const WaitHooks &hooks;
    Wake which;
    HookScope(const WaitHooks &h, Wake w) : hooks(h), which(w) {
      if (hooks.begin) hooks.begin(which);
    }
    ~HookScope() {
      if (hooks.end) hooks.end(which);
    }
  } hook_scope(hooks_, which);
  const auto account = [&] {
    if (wait_us) *wait_us += now_us() - start;
  };
  while (now_us() - start < spin_us_) {
    if (ready()) {
      account();
      return;
    }
    cpu_relax();
  }
#ifndef _WIN32
  std::uint32_t &word = wake_words_[static_cast<int>(which)];
  for (;;) {
    store32(*waiter, 1);
    const std::uint32_t seen = load32(word);  // read before the re-check: a later signal changes it
    if (ready()) {
      store32(*waiter, 0);
      break;
    }
    const int result = os_sync_wait_on_address_with_timeout(&word, seen, sizeof word, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                            OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                            static_cast<std::uint64_t>(kSleepSliceMs) * 1000000u);
    const int wait_error = errno;
    store32(*waiter, 0);
    if (ready()) break;
    if (peer_ && !process_alive(peer_pid(peer_))) {
      account();
      throw PeerExited(std::string("GS stream peer process exited while waiting for ") + what);
    }
    if (result < 0 && wait_error != ETIMEDOUT && wait_error != EINTR) {
      errno = wait_error;
      throw IpcError(last_error_text("os_sync_wait_on_address_with_timeout"));
    }
#else
  const HANDLE handles[2] = {events_[static_cast<int>(which)], peer_};
  const DWORD count = peer_ ? 2u : 1u;
  for (;;) {
    store32(*waiter, 1);
    if (ready()) {
      store32(*waiter, 0);
      break;
    }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, kSleepSliceMs);
    store32(*waiter, 0);
    if (ready()) break;
    if (result == WAIT_OBJECT_0 + 1) {
      account();
      throw PeerExited(std::string("GS stream peer process exited while waiting for ") + what);
    }
    if (result == WAIT_FAILED) throw IpcError(last_error_text("WaitForMultipleObjects"));
#endif
    if (load32(control_->state) & kStateFatal) {
      account();
      throw PeerExited(std::string("GS stream peer reported a fatal error while waiting for ") + what);
    }
    if (timeout_ms != kWaitForever && now_us() - start >= static_cast<std::uint64_t>(timeout_ms) * 1000) {
      account();
      throw TimedOut(std::string("GS stream wait for ") + what + " timed out");
    }
  }
  account();
}

void Session::post(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes, const void *second,
                   std::size_t second_bytes) {
  RingWriter &ring = commands_out();
  while (!ring.try_write(type, flags, first, first_bytes, second, second_bytes)) {
    // Back-pressure: the consumer must be awake to free space, so wake it first (batched records may be waiting).
    wake_if_waiting(&control_->data_waiter, Wake::data);
    bytes_since_data_wake_ = 0;
    // Wait until the consumer released enough bytes for the record (and a possible filler).
    const std::uint64_t need = record_size(first_bytes + second_bytes) * 2;
    ++waits_.space_waits;
    wait_until([&] { return ring.capacity() - ring.used_bytes() >= std::min(need, ring.capacity()); },
               &control_->space_waiter, Wake::space, &waits_.space_wait_us, kWaitForever, "ring space");
  }
  // TASK-041: GIF and PRIV records wake a sleeping consumer only every data_wake_bytes_ (one event signal costs a
  // system call here and a thread wake-up there); every other record type, which the producer may wait on next
  // (VSYNC frames in flight, READ_FIFO reply) or which ends the stream, wakes it at once.
  bytes_since_data_wake_ += record_size(first_bytes + second_bytes);
  const bool batchable = type == RecordType::gif || type == RecordType::priv;
  if (!batchable || data_wake_bytes_ == 0 || bytes_since_data_wake_ >= data_wake_bytes_) {
    wake_if_waiting(&control_->data_waiter, Wake::data);
    bytes_since_data_wake_ = 0;
  }
}

void Session::post_vsync(const VsyncPayload &vsync) {
  post(RecordType::vsync, 0, &vsync, sizeof vsync);
  ControlBlock &c = *control_;
  const std::uint64_t posted = load(c.vsyncs_posted, std::memory_order_relaxed) + 1;
  store(c.vsyncs_posted, posted);
  store(c.producer_heartbeat, load(c.producer_heartbeat, std::memory_order_relaxed) + 1);
  const std::uint64_t limit = c.frames_in_flight_limit;
  if (posted - load(c.vsyncs_done) > limit) {
    ++waits_.frames_waits;
    wait_until([&] { return posted - load(c.vsyncs_done) <= limit; }, &c.space_waiter, Wake::space,
               &waits_.frames_wait_us, kWaitForever, "frames in flight");
  }
}

std::uint32_t Session::read_fifo(std::uint32_t qwords, void *out) {
  ControlBlock &c = *control_;
  const std::uint64_t bytes = static_cast<std::uint64_t>(qwords) * 16;
  if (bytes > c.response_capacity) throw IpcError("READ_FIFO larger than the response area");
  const std::uint64_t id = load(c.readback_request, std::memory_order_relaxed) + 1;
  store(c.readback_request, id);
  const ReadFifoPayload request{qwords, 0, id};
  post(RecordType::read_fifo, 0, &request, sizeof request);
  wait_until([&] { return load(c.readback_done) >= id; }, &c.response_waiter, Wake::response, &waits_.response_wait_us,
             kWaitForever, "a readback reply");
  if (load(c.readback_done) != id) throw IpcError("GS stream readback reply out of order");
  const std::uint64_t reply = std::min(load(c.readback_bytes), bytes);
  std::memcpy(out, response_, static_cast<std::size_t>(reply));
  if (reply < bytes) std::memset(static_cast<std::uint8_t *>(out) + reply, 0, static_cast<std::size_t>(bytes - reply));
  return load32(c.readback_status);
}

bool Session::wait_event(RecordView &view, std::uint32_t timeout_ms) {
  RingReader &ring = events_in();
  try {
    wait_until([&] { return ring.has_data(); }, &control_->event_waiter, Wake::event, nullptr, timeout_ms, "an event");
  } catch (const TimedOut &) {
    return false;
  }
  return ring.peek(view);
}

void Session::next_command(RecordView &view) {
  RingReader &ring = commands_in();
  for (;;) {
    wait_until([&] { return ring.has_data(); }, &control_->data_waiter, Wake::data, &waits_.data_wait_us, kWaitForever,
               "commands");
    if (ring.peek(view)) return;
  }
}

void Session::release_command(const RecordView &view) {
  commands_in().release(view);
  wake_if_waiting(&control_->space_waiter, Wake::space);
}

void Session::complete_vsync() {
  ControlBlock &c = *control_;
  store(c.vsyncs_done, load(c.vsyncs_done, std::memory_order_relaxed) + 1);
  wake_if_waiting(&c.space_waiter, Wake::space);
}

void Session::complete_readback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) {
  ControlBlock &c = *control_;
  store(c.readback_bytes, bytes);
  store32(c.readback_status, status);
  store(c.readback_done, request_id);
  wake_if_waiting(&c.response_waiter, Wake::response);
}

void Session::post_event(RecordType type, const void *payload, std::size_t bytes) {
  RingWriter &ring = events_out();
  // The event ring holds a few hundred records and the producer drains it every VSYNC; a full ring is rare, so a
  // short sleep is enough. A dead producer ends the wait.
  while (!ring.try_write(type, 0, payload, bytes, nullptr, 0)) {
#ifndef _WIN32
    if (peer_ && !process_alive(peer_pid(peer_))) throw PeerExited("GS stream producer exited");
    usleep(1000);
#else
    if (peer_ && WaitForSingleObject(peer_, 1) == WAIT_OBJECT_0) throw PeerExited("GS stream producer exited");
    if (!peer_) Sleep(1);
#endif
  }
  wake_if_waiting(&control_->event_waiter, Wake::event);
}

// ---------------------------------------------------------------------------------------------------------------
// Child process
// ---------------------------------------------------------------------------------------------------------------

#ifndef _WIN32
// POSIX: process_ holds the child's pid; job_ is unused. There is no kill-on-close job: the destructor kills a child
// that still runs, and children still running when this process exits normally (std::exit, return from main) are
// killed by an atexit handler, which covers the client's refusals. A child of a process that is itself killed is not
// ended by anyone: a GS process watches its parent pid (--parent) instead.
namespace {

std::mutex g_children_mutex;
std::vector<pid_t> g_children;  // started by spawn, not yet reaped

void kill_children_at_exit() {
  std::lock_guard<std::mutex> lock(g_children_mutex);
  for (pid_t pid : g_children) {
    ::kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
  g_children.clear();
}

void track_child(pid_t pid) {
  static const bool registered = std::atexit(kill_children_at_exit) == 0;
  (void)registered;
  std::lock_guard<std::mutex> lock(g_children_mutex);
  g_children.push_back(pid);
}

void forget_child(pid_t pid) {
  std::lock_guard<std::mutex> lock(g_children_mutex);
  g_children.erase(std::remove(g_children.begin(), g_children.end(), pid), g_children.end());
}

}  // namespace

ChildProcess::~ChildProcess() {
  if (process_ && running()) kill();
  if (process_ && waitpid(peer_pid(process_), nullptr, WNOHANG) == peer_pid(process_)) forget_child(peer_pid(process_));
}

ChildProcess::ChildProcess(ChildProcess &&other) noexcept
    : process_(other.process_), job_(other.job_), pid_(other.pid_) {
  other.process_ = other.job_ = nullptr;
  other.pid_ = 0;
}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept {
  if (this != &other) {
    if (process_ && running()) kill();
    process_ = other.process_;
    job_ = other.job_;
    pid_ = other.pid_;
    other.process_ = other.job_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ChildProcess ChildProcess::spawn(const std::wstring &executable, const std::wstring &arguments) {
  const std::string path = narrow(executable);
  std::vector<std::string> parts = split_arguments(narrow(arguments));
  parts.insert(parts.begin(), path);
  std::vector<char *> argv;
  for (std::string &part : parts) argv.push_back(part.data());
  argv.push_back(nullptr);
  // Standard input, output and error are inherited, so the child's log lines reach the same output.
  pid_t pid = 0;
  const int result = posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv.data(), environ);
  if (result != 0) {
    errno = result;
    throw IpcError(last_error_text("posix_spawn") + " for " + path);
  }
  track_child(pid);
  ChildProcess child;
  child.pid_ = static_cast<std::uint32_t>(pid);
  child.process_ = reinterpret_cast<NativeHandle>(static_cast<std::uintptr_t>(pid));
  return child;
}

bool ChildProcess::running() const { return process_ && process_alive(peer_pid(process_)); }

bool ChildProcess::wait_exit(std::uint32_t timeout_ms, std::uint32_t *exit_code) const {
  if (!process_) return true;
  const pid_t pid = peer_pid(process_);
  const std::uint64_t start = now_us();
  for (;;) {
    siginfo_t info{};
    if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid) {
      if (exit_code)
        *exit_code = info.si_code == CLD_EXITED ? static_cast<std::uint32_t>(info.si_status)
                                                : 128u + static_cast<std::uint32_t>(info.si_status);
      return true;
    }
    if (timeout_ms != kWaitForever && now_us() - start >= static_cast<std::uint64_t>(timeout_ms) * 1000) return false;
    usleep(1000);
  }
}

void ChildProcess::kill() {
  if (!process_) return;
  ::kill(peer_pid(process_), SIGKILL);
  wait_exit(5000);
}
#else

ChildProcess::~ChildProcess() {
  if (job_) CloseHandle(job_);  // kill-on-close ends the child if it still runs
  close_handle(process_);
}

ChildProcess::ChildProcess(ChildProcess &&other) noexcept
    : process_(other.process_), job_(other.job_), pid_(other.pid_) {
  other.process_ = other.job_ = nullptr;
  other.pid_ = 0;
}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept {
  if (this != &other) {
    if (job_) CloseHandle(job_);
    close_handle(process_);
    process_ = other.process_;
    job_ = other.job_;
    pid_ = other.pid_;
    other.process_ = other.job_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ChildProcess ChildProcess::spawn(const std::wstring &executable, const std::wstring &arguments) {
  ChildProcess child;
  child.job_ = CreateJobObjectW(nullptr, nullptr);
  if (!child.job_) throw IpcError(last_error_text("CreateJobObjectW"));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(child.job_, JobObjectExtendedLimitInformation, &limits, sizeof limits))
    throw IpcError(last_error_text("SetInformationJobObject"));

  // The child shares our standard handles (inheritable duplicates), so its log lines reach the same output.
  HANDLE inherited[3] = {};
  const DWORD kinds[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int index = 0; index < 3; ++index) {
    const HANDLE original = GetStdHandle(kinds[index]);
    if (original && original != INVALID_HANDLE_VALUE)
      DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &inherited[index], 0, TRUE,
                      DUPLICATE_SAME_ACCESS);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = inherited[0];
  startup.hStdOutput = inherited[1];
  startup.hStdError = inherited[2];
  std::wstring command_line = L"\"" + executable + L"\" " + arguments;
  PROCESS_INFORMATION info{};
  const BOOL created = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                      CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
  const DWORD create_error = GetLastError();
  for (HANDLE handle : inherited) close_handle(handle);
  if (!created) {
    SetLastError(create_error);
    throw IpcError(last_error_text("CreateProcessW"));
  }
  child.process_ = info.hProcess;
  child.pid_ = info.dwProcessId;
  if (!AssignProcessToJobObject(child.job_, info.hProcess)) {
    const std::string text = last_error_text("AssignProcessToJobObject");
    TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hThread);
    throw IpcError(text);
  }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  return child;
}

bool ChildProcess::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

bool ChildProcess::wait_exit(std::uint32_t timeout_ms, std::uint32_t *exit_code) const {
  if (!process_) return true;
  if (WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) return false;
  if (exit_code) {
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    *exit_code = code;
  }
  return true;
}

void ChildProcess::kill() {
  if (job_) TerminateJobObject(job_, 1);
  if (process_) WaitForSingleObject(process_, 5000);
}

#endif

}  // namespace gs_stream::ipc
