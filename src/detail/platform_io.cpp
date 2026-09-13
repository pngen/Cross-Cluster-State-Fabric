// Cross-Cluster State Fabric - durable file primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "detail/platform_io.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ccsf::detail {
namespace {

Error io_error(const std::string& message, const std::string& path) {
  return Error(ErrorCode::PERSISTENCE_IO, message, path);
}

#if defined(_WIN32)

std::wstring to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int required = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                            static_cast<int>(text.size()), wide.data(), required);
  if (written != required) {
    return std::wstring();
  }
  return wide;
}

Status write_all(HANDLE handle, ByteSpan data, const std::string& path) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    const DWORD chunk = remaining > 0x40000000ULL ? 0x40000000U
                                                  : static_cast<DWORD>(remaining);
    DWORD written = 0;
    if (::WriteFile(handle, data.data() + offset, chunk, &written, nullptr) == 0) {
      return io_error("WriteFile failed with error " + std::to_string(::GetLastError()), path);
    }
    if (written == 0) {
      return io_error("WriteFile reported no progress", path);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::FlushFileBuffers(handle) == 0) {
    return io_error("FlushFileBuffers failed with error " + std::to_string(::GetLastError()),
                    path);
  }
  return Status{};
}

#else

std::string errno_text(const char* what) {
  return std::string(what) + " failed: " + std::strerror(errno);
}

Status write_all(int fd, ByteSpan data, const std::string& path) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return io_error(errno_text("write"), path);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    return io_error(errno_text("fsync"), path);
  }
  return Status{};
}

#endif

}  // namespace

std::string join_path(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  const char last = directory.back();
  if (last == '/' || last == '\\') {
    return directory + name;
  }
  return directory + "/" + name;
}

Result<bool> file_exists(const std::string& path) {
  if (path.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "empty path");
  }
  std::error_code code;
  const bool exists = std::filesystem::exists(std::filesystem::path(path), code);
  if (code) {
    return io_error("existence check failed: " + code.message(), path);
  }
  return exists;
}

Result<std::uint64_t> file_size(const std::string& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), code);
  if (code) {
    return io_error("file size query failed: " + code.message(), path);
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::vector<std::byte>> read_file(const std::string& path) {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), code);
  if (code) {
    return io_error("file size query failed: " + code.message(), path);
  }
  if (size > (1ULL << 32U)) {
    return Error(ErrorCode::RESOURCE_EXHAUSTED,
                 "control plane file exceeds the bounded read limit", path);
  }
  std::FILE* file = nullptr;
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "path is not valid UTF-8: " + path);
  }
  if (::_wfopen_s(&file, wide.c_str(), L"rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (file == nullptr) {
    return io_error("open for read failed", path);
  }
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  std::size_t read_bytes = 0;
  if (!data.empty()) {
    read_bytes = std::fread(data.data(), 1, data.size(), file);
  }
  const bool short_read = read_bytes != data.size();
  std::fclose(file);
  if (short_read) {
    return Error(ErrorCode::PERSISTENCE_TRUNCATED, "short read while loading file", path);
  }
  return data;
}

Status ensure_directory(const std::string& path) {
  std::error_code code;
  const auto status = std::filesystem::status(std::filesystem::path(path), code);
  if (!code && std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status)) {
      return Error(ErrorCode::PERSISTENCE_IO, "path exists and is not a directory", path);
    }
    return Status{};
  }
  code.clear();
  std::filesystem::create_directories(std::filesystem::path(path), code);
  if (code) {
    return io_error("directory creation failed: " + code.message(), path);
  }
  return Status{};
}

Status write_file_durable(const std::string& path, ByteSpan data) {
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "path is not valid UTF-8: " + path);
  }
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("CreateFileW(CREATE_ALWAYS) failed with error " +
                        std::to_string(::GetLastError()),
                    path);
  }
  Status status = write_all(handle, data, path);
  ::CloseHandle(handle);
  return status;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return io_error(errno_text("open"), path);
  }
  Status status = write_all(fd, data, path);
  ::close(fd);
  return status;
#endif
}

Status append_file_durable(const std::string& path, ByteSpan data) {
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "path is not valid UTF-8: " + path);
  }
  HANDLE handle = ::CreateFileW(wide.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("CreateFileW(OPEN_ALWAYS) failed with error " +
                        std::to_string(::GetLastError()),
                    path);
  }
  Status status = write_all(handle, data, path);
  ::CloseHandle(handle);
  return status;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return io_error(errno_text("open"), path);
  }
  Status status = write_all(fd, data, path);
  ::close(fd);
  return status;
#endif
}

Status atomic_replace(const std::string& source, const std::string& target) {
#if defined(_WIN32)
  const std::wstring wide_source = to_wide(source);
  const std::wstring wide_target = to_wide(target);
  if (wide_source.empty() || wide_target.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "path is not valid UTF-8");
  }
  if (::MoveFileExW(wide_source.c_str(), wide_target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return io_error("MoveFileExW failed with error " + std::to_string(::GetLastError()),
                    target);
  }
  return Status{};
#else
  if (::rename(source.c_str(), target.c_str()) != 0) {
    return io_error(errno_text("rename"), target);
  }
  const std::string directory = target.substr(0, target.find_last_of('/'));
  if (!directory.empty()) {
    const int dir_fd = ::open(directory.c_str(), O_RDONLY);
    if (dir_fd >= 0) {
      (void)::fsync(dir_fd);
      ::close(dir_fd);
    }
  }
  return Status{};
#endif
}

Status remove_file(const std::string& path) {
  std::error_code code;
  std::filesystem::remove(std::filesystem::path(path), code);
  if (code) {
    return io_error("file removal failed: " + code.message(), path);
  }
  return Status{};
}

}  // namespace ccsf::detail
