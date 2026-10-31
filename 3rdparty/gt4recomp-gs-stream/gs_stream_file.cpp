// SPDX-License-Identifier: MIT
// GS stream files (.gscap version 2); see gs_stream_file.hpp.
#include "gs_stream_file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
namespace {
// The Windows CRT call this file uses; local so the file is self-contained when vendored (gt4recomp-gs).
int fopen_s(std::FILE **file, const char *path, const char *mode) {
  *file = std::fopen(path, mode);
  return *file ? 0 : errno;
}
} // namespace
#endif

namespace gs_stream {

FileWriter::~FileWriter() { close(); }

bool FileWriter::open(const std::string &path, const std::string &serial, std::uint64_t first_frame, std::string &error) {
  close();
  if (serial.size() > 15) {
    error = "serial longer than 15 characters";
    return false;
  }
  if (fopen_s(&file_, path.c_str(), "wb") != 0 || !file_) {
    file_ = nullptr;
    error = "cannot create " + path;
    return false;
  }
  buffer_.resize(1u << 20);
  std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size());
  FileHeader header{};
  std::memcpy(header.magic, kFileMagic, sizeof header.magic);
  header.file_version = kFileVersion;
  header.protocol_major = kVersionMajor;
  header.protocol_minor = kVersionMinor;
  header.header_bytes = sizeof(FileHeader);
  header.record_header_bytes = sizeof(RecordHeader);
  std::memcpy(header.serial, serial.data(), serial.size());
  header.first_frame = first_frame;
  std::fwrite(&header, sizeof header, 1, file_);
  records_ = 0;
  bytes_ = sizeof header;
  return true;
}

void FileWriter::write(RecordType type, std::uint16_t flags, std::uint64_t sequence, const void *first,
                       std::size_t first_bytes, const void *second, std::size_t second_bytes) {
  if (!file_) return;
  const std::uint64_t payload = static_cast<std::uint64_t>(first_bytes) + second_bytes;
  RecordHeader header{};
  header.type = static_cast<std::uint16_t>(type);
  header.flags = flags;
  header.payload_bytes = static_cast<std::uint32_t>(payload);
  header.sequence = sequence;
  std::fwrite(&header, sizeof header, 1, file_);
  if (first_bytes) std::fwrite(first, 1, first_bytes, file_);
  if (second_bytes) std::fwrite(second, 1, second_bytes, file_);
  static const std::uint8_t kZeros[kRecordAlign] = {};
  const std::uint64_t padding = align_record(payload) - payload;
  if (padding) std::fwrite(kZeros, 1, static_cast<std::size_t>(padding), file_);
  ++records_;
  bytes_ += record_size(payload);
}

void FileWriter::write_raw(const void *records, std::size_t bytes) {
  if (!file_) return;
  std::fwrite(records, 1, bytes, file_);
  ++records_;
  bytes_ += bytes;
}

void FileWriter::flush() {
  if (file_) std::fflush(file_);
}

void FileWriter::close() {
  if (!file_) return;
  std::fclose(file_);
  file_ = nullptr;
}

FileReader::~FileReader() {
  if (file_) std::fclose(file_);
}

bool FileReader::open(const std::string &path, std::string &error) {
  if (fopen_s(&file_, path.c_str(), "rb") != 0 || !file_) {
    file_ = nullptr;
    error = "cannot open " + path;
    return false;
  }
  if (std::fread(&header_, sizeof header_, 1, file_) != 1 || std::memcmp(header_.magic, kFileMagic, 8) != 0 ||
      header_.file_version != kFileVersion || header_.header_bytes != sizeof(FileHeader) ||
      header_.record_header_bytes != sizeof(RecordHeader)) {
    error = path + " is not a version 2 GS stream file";
    return false;
  }
  if (header_.protocol_major != kVersionMajor) {
    error = path + " uses another protocol major version";
    return false;
  }
  return true;
}

bool FileReader::next(RecordHeader &header, std::vector<std::uint8_t> &payload) {
  const std::size_t got = std::fread(&header, 1, sizeof header, file_);
  if (got == 0) return false;
  if (got != sizeof header) throw std::runtime_error("truncated record header");
  if (header.type == static_cast<std::uint16_t>(RecordType::wrap) || header.sequence == 0)
    throw std::runtime_error("malformed record header in a stream file");
  payload.resize(static_cast<std::size_t>(align_record(header.payload_bytes)));
  if (!payload.empty() && std::fread(payload.data(), 1, payload.size(), file_) != payload.size())
    throw std::runtime_error("truncated record payload");
  payload.resize(header.payload_bytes);
  return true;
}

}  // namespace gs_stream
