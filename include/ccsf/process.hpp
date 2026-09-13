// Cross-Cluster State Fabric - child process management.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The multiprocess proof needs real, independent operating system processes
// with hidden windows, inherited output pipes and hard termination. Readiness
// synchronization is done through explicit line reads that block on a condition
// variable: no polling, no sleeps and no timeouts.

#ifndef CCSF_PROCESS_HPP
#define CCSF_PROCESS_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ccsf/result.hpp"

namespace ccsf {

struct ProcessOptions {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;

  /// Captures the child's standard output into an internal transcript.
  bool capture_output{true};

  /// Extra environment entries in "NAME=VALUE" form.
  std::vector<std::string> environment;
};

/// A running child process.
class ChildProcess {
 public:
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Spawns the child with a hidden window and no console window.
  static Result<std::unique_ptr<ChildProcess>> spawn(const ProcessOptions& options);

  /// Blocks until the child prints a line accepted by the predicate, and
  /// returns it. Passing an empty predicate accepts the next line. Returns
  /// TRANSPORT_CLOSED when the child's output ends first.
  Result<std::string> wait_for_line(const std::function<bool(const std::string&)>& predicate);

  /// Returns the next line if one is already buffered.
  bool try_read_line(std::string* out);

  /// Blocks until the child exits and returns its exit code.
  Result<int> wait();

  [[nodiscard]] bool running() const;
  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }

  /// Hard termination, used to simulate a crash. No cleanup runs in the child.
  Status kill();

  /// Every line the child has produced so far.
  [[nodiscard]] std::vector<std::string> transcript() const;

  /// Blocks until the child's output stream ends.
  void wait_for_output_end();

 private:
  ChildProcess() = default;
  void start_reader();

  std::uintptr_t process_handle_{0};
  std::uintptr_t thread_handle_{0};
  std::uintptr_t pipe_read_{0};
  std::uint32_t process_id_{0};
  bool capture_output_{true};

  struct OutputState;
  std::shared_ptr<OutputState> output_;
  std::thread reader_thread_;
};

/// Quotes one argument according to the Windows command line rules.
std::string quote_argument(const std::string& argument);

/// Builds a complete command line from an executable and its arguments.
std::string build_command_line(const std::string& executable,
                               const std::vector<std::string>& arguments);

}  // namespace ccsf

#endif  // CCSF_PROCESS_HPP
