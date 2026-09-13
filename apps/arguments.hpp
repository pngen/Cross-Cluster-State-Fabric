// Cross-Cluster State Fabric - reference deployment argument parsing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_APPS_ARGUMENTS_HPP
#define CCSF_APPS_ARGUMENTS_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ccsf::app {

/// Minimal deterministic argument list: "--name value" pairs and flags.
struct Arguments {
  std::vector<std::string> items;

  [[nodiscard]] bool has(const std::string& name) const {
    for (const std::string& item : items) {
      if (item == name) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string value(const std::string& name, const std::string& fallback) const {
    for (std::size_t index = 0; index + 1U < items.size(); ++index) {
      if (items[index] == name) {
        return items[index + 1U];
      }
    }
    return fallback;
  }

  [[nodiscard]] std::uint64_t number(const std::string& name, std::uint64_t fallback) const {
    const std::string text = value(name, std::string());
    if (text.empty()) {
      return fallback;
    }
    return std::strtoull(text.c_str(), nullptr, 10);
  }

  [[nodiscard]] std::vector<std::string> repeated(const std::string& name) const {
    std::vector<std::string> values;
    for (std::size_t index = 0; index + 1U < items.size(); ++index) {
      if (items[index] == name) {
        values.push_back(items[index + 1U]);
      }
    }
    return values;
  }
};

/// Appends observability lines to a file, flushing each line so that an
/// operator (or a test harness) can observe a live process.
class TraceFile {
 public:
  explicit TraceFile(const std::string& path) {
    if (path.empty()) {
      return;
    }
#if defined(_WIN32)
    if (::fopen_s(&file_, path.c_str(), "ab") != 0) {
      file_ = nullptr;
    }
#else
    file_ = std::fopen(path.c_str(), "ab");
#endif
  }

  ~TraceFile() {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  TraceFile(const TraceFile&) = delete;
  TraceFile& operator=(const TraceFile&) = delete;

  void line(const std::string& text) {
    if (file_ == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    std::fprintf(file_, "%s\n", text.c_str());
    std::fflush(file_);
  }

  [[nodiscard]] bool enabled() const noexcept { return file_ != nullptr; }

 private:
  std::FILE* file_{nullptr};
  std::mutex mutex_;
};

/// Shared pointer wrapper so the trace sink outlives the runtime that uses it.
using TraceHandle = std::shared_ptr<TraceFile>;

inline std::function<void(const std::string&)> trace_callback(const TraceHandle& handle) {
  if (handle == nullptr || !handle->enabled()) {
    return {};
  }
  return [handle](const std::string& text) { handle->line(text); };
}

inline Arguments parse(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.items.emplace_back(argv[index]);
  }
  return arguments;
}

}  // namespace ccsf::app

#endif  // CCSF_APPS_ARGUMENTS_HPP
