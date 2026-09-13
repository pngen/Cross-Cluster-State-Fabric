// Cross-Cluster State Fabric - durable file primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable control plane depends on three primitives: a flush that reaches
// stable storage, an append that is ordered, and a replacement that is atomic
// on Windows without relying on POSIX rename semantics.

#ifndef CCSF_DETAIL_PLATFORM_IO_HPP
#define CCSF_DETAIL_PLATFORM_IO_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/bytes.hpp"
#include "ccsf/result.hpp"

namespace ccsf::detail {

std::string join_path(const std::string& directory, const std::string& name);

Result<bool> file_exists(const std::string& path);
Result<std::uint64_t> file_size(const std::string& path);
Result<std::vector<std::byte>> read_file(const std::string& path);

/// Creates the directory (and parents) if it does not exist.
Status ensure_directory(const std::string& path);

/// Writes the whole file, flushes it to stable storage and closes it.
Status write_file_durable(const std::string& path, ByteSpan data);

/// Appends bytes, flushing them to stable storage before returning.
Status append_file_durable(const std::string& path, ByteSpan data);

/// Atomically replaces "target" with the contents of "source". On Windows this
/// uses MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
Status atomic_replace(const std::string& source, const std::string& target);

Status remove_file(const std::string& path);

}  // namespace ccsf::detail

#endif  // CCSF_DETAIL_PLATFORM_IO_HPP
