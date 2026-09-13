// Cross-Cluster State Fabric - checked binary codec implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/bytes.hpp"

#include <limits>

namespace ccsf {

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t* out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  *out = a + b;
  return true;
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t* out) noexcept {
  if (a != 0U && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  *out = a * b;
  return true;
}

void ByteWriter::u8(std::uint8_t value) {
  data_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::u16(std::uint16_t value) {
  data_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  data_.push_back(static_cast<std::byte>(value & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
  data_.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  data_.push_back(static_cast<std::byte>(value & 0xFFU));
}

void ByteWriter::u64(std::uint64_t value) {
  data_.push_back(static_cast<std::byte>((value >> 56U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 48U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 40U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 32U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
  data_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  data_.push_back(static_cast<std::byte>(value & 0xFFU));
}

void ByteWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void ByteWriter::boolean(bool value) {
  u8(value ? 1U : 0U);
}

void ByteWriter::raw(ByteSpan bytes) {
  data_.insert(data_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::blob(ByteSpan bytes) {
  const std::size_t size = bytes.size();
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    // Callers bound their own inputs; a larger blob is a programming error and
    // is truncated loudly rather than silently producing an ambiguous frame.
    u32(std::numeric_limits<std::uint32_t>::max());
    raw(bytes.first(std::numeric_limits<std::uint32_t>::max()));
    return;
  }
  u32(static_cast<std::uint32_t>(size));
  raw(bytes);
}

void ByteWriter::str(std::string_view text) {
  blob(ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

bool ByteReader::u8(std::uint8_t* out) {
  if (!ok_) {
    return false;
  }
  if (position_ + 1U > data_.size()) {
    fail(ErrorCode::INVALID_ARGUMENT, "truncated u8");
    return false;
  }
  *out = std::to_integer<std::uint8_t>(data_[position_]);
  position_ += 1U;
  return true;
}

bool ByteReader::u16(std::uint16_t* out) {
  if (!ok_) {
    return false;
  }
  if (position_ + 2U > data_.size()) {
    fail(ErrorCode::INVALID_ARGUMENT, "truncated u16");
    return false;
  }
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    value = static_cast<std::uint16_t>(value << 8U);
    value = static_cast<std::uint16_t>(
        value | std::to_integer<std::uint8_t>(data_[position_ + static_cast<std::size_t>(index)]));
  }
  position_ += 2U;
  *out = value;
  return true;
}

bool ByteReader::u32(std::uint32_t* out) {
  if (!ok_) {
    return false;
  }
  if (position_ + 4U > data_.size()) {
    fail(ErrorCode::INVALID_ARGUMENT, "truncated u32");
    return false;
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8U) |
            std::to_integer<std::uint8_t>(data_[position_ + static_cast<std::size_t>(index)]);
  }
  position_ += 4U;
  *out = value;
  return true;
}

bool ByteReader::u64(std::uint64_t* out) {
  if (!ok_) {
    return false;
  }
  if (position_ + 8U > data_.size()) {
    fail(ErrorCode::INVALID_ARGUMENT, "truncated u64");
    return false;
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8U) |
            std::to_integer<std::uint8_t>(data_[position_ + static_cast<std::size_t>(index)]);
  }
  position_ += 8U;
  *out = value;
  return true;
}

bool ByteReader::i64(std::int64_t* out) {
  std::uint64_t raw = 0;
  if (!u64(&raw)) {
    return false;
  }
  *out = static_cast<std::int64_t>(raw);
  return true;
}

bool ByteReader::boolean(bool* out) {
  std::uint8_t raw = 0;
  if (!u8(&raw)) {
    return false;
  }
  if (raw > 1U) {
    fail(ErrorCode::INVALID_ARGUMENT, "invalid boolean encoding");
    return false;
  }
  *out = raw == 1U;
  return true;
}

bool ByteReader::raw(std::size_t count, ByteSpan* out) {
  if (!ok_) {
    return false;
  }
  std::uint64_t end = 0;
  if (!checked_add(static_cast<std::uint64_t>(position_), static_cast<std::uint64_t>(count), &end) ||
      end > static_cast<std::uint64_t>(data_.size())) {
    fail(ErrorCode::INVALID_ARGUMENT, "truncated raw byte range");
    return false;
  }
  *out = data_.subspan(position_, count);
  position_ += count;
  return true;
}

bool ByteReader::blob(std::size_t max_length, std::vector<std::byte>* out) {
  std::uint32_t length = 0;
  if (!u32(&length)) {
    return false;
  }
  if (static_cast<std::size_t>(length) > max_length) {
    fail(ErrorCode::RESOURCE_EXHAUSTED, "declared blob length exceeds the permitted bound");
    return false;
  }
  ByteSpan view;
  if (!raw(static_cast<std::size_t>(length), &view)) {
    return false;
  }
  out->assign(view.begin(), view.end());
  return true;
}

bool ByteReader::str(std::size_t max_length, std::string* out) {
  std::uint32_t length = 0;
  if (!u32(&length)) {
    return false;
  }
  if (static_cast<std::size_t>(length) > max_length) {
    fail(ErrorCode::RESOURCE_EXHAUSTED, "declared string length exceeds the permitted bound");
    return false;
  }
  ByteSpan view;
  if (!raw(static_cast<std::size_t>(length), &view)) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(view.data()), view.size());
  return true;
}

bool ByteReader::require_end() {
  if (!ok_) {
    return false;
  }
  if (position_ != data_.size()) {
    fail(ErrorCode::INVALID_ARGUMENT, "trailing bytes after the final field");
    return false;
  }
  return true;
}

void ByteReader::fail(ErrorCode code, std::string message) {
  if (ok_) {
    ok_ = false;
    error_ = Error(code, std::move(message));
  }
}

std::string to_string(ByteSpan bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

ByteSpan as_bytes(std::string_view text) {
  return ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

}  // namespace ccsf
