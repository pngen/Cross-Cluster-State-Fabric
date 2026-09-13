// Cross-Cluster State Fabric - checked binary encoding helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// All persistent and wire encodings in this runtime are big-endian with
// explicitly checked lengths. Readers are total: every accessor reports
// failure through a sticky error instead of reading out of bounds.

#ifndef CCSF_BYTES_HPP
#define CCSF_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ccsf/error.hpp"

namespace ccsf {

using ByteSpan = std::span<const std::byte>;
using MutableByteSpan = std::span<std::byte>;

/// Checked unsigned addition. Returns false on overflow.
bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t* out) noexcept;

/// Checked unsigned multiplication. Returns false on overflow.
bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t* out) noexcept;

/// Appends big-endian primitives and length-prefixed byte strings.
class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);

  /// Raw bytes without a length prefix.
  void raw(ByteSpan bytes);

  /// 32-bit length prefix followed by the bytes.
  void blob(ByteSpan bytes);

  /// 32-bit length prefix followed by the UTF-8 bytes of the string.
  void str(std::string_view text);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  void clear() noexcept { data_.clear(); }

 private:
  std::vector<std::byte> data_;
};

/// Bounds-checked big-endian reader with a sticky error.
class ByteReader {
 public:
  ByteReader() = default;
  explicit ByteReader(ByteSpan data) : data_(data) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return ok_ ? data_.size() - position_ : 0U;
  }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] bool at_end() const noexcept { return ok_ && position_ == data_.size(); }

  bool u8(std::uint8_t* out);
  bool u16(std::uint16_t* out);
  bool u32(std::uint32_t* out);
  bool u64(std::uint64_t* out);
  bool i64(std::int64_t* out);
  bool boolean(bool* out);

  /// Raw bytes without a length prefix.
  bool raw(std::size_t count, ByteSpan* out);

  /// 32-bit length prefix followed by that many bytes; rejects lengths above max_length.
  bool blob(std::size_t max_length, std::vector<std::byte>* out);

  /// 32-bit length prefixed UTF-8 string; rejects lengths above max_length.
  bool str(std::size_t max_length, std::string* out);

  /// Fails unless every byte has been consumed.
  bool require_end();

  void fail(ErrorCode code, std::string message);

 private:
  ByteSpan data_{};
  std::size_t position_{0};
  bool ok_{true};
  Error error_{};
};

/// Copies bytes into a std::string payload.
std::string to_string(ByteSpan bytes);
ByteSpan as_bytes(std::string_view text);

}  // namespace ccsf

#endif  // CCSF_BYTES_HPP
