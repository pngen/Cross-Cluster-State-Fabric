// Cross-Cluster State Fabric - child process implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/process.hpp"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ccsf {

std::string quote_argument(const std::string& argument) {
  if (argument.empty()) {
    return "\"\"";
  }
  const bool needs_quotes =
      argument.find_first_of(" \t\n\v\"") != std::string::npos;
  if (!needs_quotes) {
    return argument;
  }
  std::string quoted = "\"";
  std::size_t backslashes = 0;
  for (char character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(backslashes * 2U + 1U, '\\');
      quoted.push_back('"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2U, '\\');
  quoted.push_back('"');
  return quoted;
}

std::string build_command_line(const std::string& executable,
                               const std::vector<std::string>& arguments) {
  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command += quote_argument(argument);
  }
  return command;
}

struct ChildProcess::OutputState {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::string> pending;
  std::vector<std::string> transcript;
  bool ended{false};
  std::string partial;
};

#if defined(_WIN32)

namespace {

struct HandleCloser {
  void operator()(void* handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(static_cast<HANDLE>(handle));
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

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
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                        required);
  return wide;
}

}  // namespace

ChildProcess::~ChildProcess() {
  // A still-running child is terminated before teardown so that its output pipe
  // reaches end of file and the reader thread can always be joined. Waiting on a
  // detached native thread handle would be undefined behaviour: detach() closes
  // it.
  if (process_handle_ != 0 && running()) {
    (void)::TerminateProcess(reinterpret_cast<HANDLE>(process_handle_), 1);
    ::WaitForSingleObject(reinterpret_cast<HANDLE>(process_handle_), INFINITE);
  }
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
  if (thread_handle_ != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(thread_handle_));
  }
  if (process_handle_ != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(process_handle_));
  }
  if (pipe_read_ != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(pipe_read_));
  }
}

void ChildProcess::start_reader() {
  std::shared_ptr<OutputState> state = output_;
  HANDLE pipe = reinterpret_cast<HANDLE>(pipe_read_);
  reader_thread_ = std::thread([state, pipe]() {
    char buffer[4096];
    for (;;) {
      DWORD read = 0;
      if (::ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) == 0 || read == 0) {
        break;
      }
      std::lock_guard<std::mutex> guard(state->mutex);
      for (DWORD index = 0; index < read; ++index) {
        const char character = buffer[index];
        if (character == '\n') {
          if (!state->partial.empty() && state->partial.back() == '\r') {
            state->partial.pop_back();
          }
          state->pending.push_back(state->partial);
          state->transcript.push_back(state->partial);
          state->partial.clear();
        } else {
          state->partial.push_back(character);
        }
      }
      state->condition.notify_all();
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    if (!state->partial.empty()) {
      state->pending.push_back(state->partial);
      state->transcript.push_back(state->partial);
      state->partial.clear();
    }
    state->ended = true;
    state->condition.notify_all();
  });
}

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const ProcessOptions& options) {
  if (options.executable.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "an executable path is required");
  }
  std::unique_ptr<ChildProcess> child(new ChildProcess());
  child->capture_output_ = options.capture_output;
  if (options.capture_output) {
    child->output_ = std::make_shared<OutputState>();
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (options.capture_output) {
    if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
      return Error(ErrorCode::INTERNAL_ERROR, "CreatePipe failed",
                   std::to_string(::GetLastError()));
    }
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  if (options.capture_output) {
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  }
  PROCESS_INFORMATION information{};

  const std::wstring wide_executable = to_wide(options.executable);
  if (wide_executable.empty()) {
    if (options.capture_output) {
      ::CloseHandle(read_end);
      ::CloseHandle(write_end);
    }
    return make_error(ErrorCode::INVALID_ARGUMENT, "the executable path is not valid UTF-8");
  }
  std::string command = build_command_line(options.executable, options.arguments);
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  const std::wstring wide_directory = to_wide(options.working_directory);

  std::string environment_block;
  if (!options.environment.empty()) {
    for (const std::string& entry : options.environment) {
      environment_block += entry;
      environment_block.push_back('\0');
    }
    environment_block.push_back('\0');
  }

  DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  const BOOL created = ::CreateProcessW(
      wide_executable.c_str(), command_buffer.data(), nullptr, nullptr,
      options.capture_output ? TRUE : FALSE, flags, nullptr,
      wide_directory.empty() ? nullptr : wide_directory.c_str(), &startup, &information);
  if (options.capture_output) {
    ::CloseHandle(write_end);
  }
  if (created == 0) {
    if (options.capture_output) {
      ::CloseHandle(read_end);
    }
    return Error(ErrorCode::INTERNAL_ERROR, "CreateProcessW failed",
                 std::to_string(::GetLastError()));
  }
  child->process_handle_ = reinterpret_cast<std::uintptr_t>(information.hProcess);
  child->thread_handle_ = reinterpret_cast<std::uintptr_t>(information.hThread);
  child->process_id_ = information.dwProcessId;
  if (options.capture_output) {
    child->pipe_read_ = reinterpret_cast<std::uintptr_t>(read_end);
    child->start_reader();
  }
  return child;
}

Result<std::string> ChildProcess::wait_for_line(
    const std::function<bool(const std::string&)>& predicate) {
  if (!output_) {
    return make_error(ErrorCode::NOT_SUPPORTED, "output capture is disabled");
  }
  std::shared_ptr<OutputState> state = output_;
  std::unique_lock<std::mutex> guard(state->mutex);
  for (;;) {
    for (std::size_t index = 0; index < state->pending.size(); ++index) {
      const std::string& line = state->pending[index];
      if (!predicate || predicate(line)) {
        std::string matched = line;
        state->pending.erase(state->pending.begin() + static_cast<std::ptrdiff_t>(index));
        return matched;
      }
    }
    if (state->ended && state->pending.empty()) {
      return make_error(ErrorCode::TRANSPORT_CLOSED, "the child closed its output stream");
    }
    state->condition.wait(guard);
  }
}

bool ChildProcess::try_read_line(std::string* out) {
  if (!output_) {
    return false;
  }
  std::shared_ptr<OutputState> state = output_;
  std::lock_guard<std::mutex> guard(state->mutex);
  if (state->pending.empty()) {
    return false;
  }
  *out = state->pending.front();
  state->pending.pop_front();
  return true;
}

void ChildProcess::wait_for_output_end() {
  if (!output_) {
    return;
  }
  std::shared_ptr<OutputState> state = output_;
  std::unique_lock<std::mutex> guard(state->mutex);
  while (!state->ended) {
    state->condition.wait(guard);
  }
}

Result<int> ChildProcess::wait() {
  if (process_handle_ == 0) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "the process handle is closed");
  }
  ::WaitForSingleObject(reinterpret_cast<HANDLE>(process_handle_), INFINITE);
  DWORD code = 0;
  if (::GetExitCodeProcess(reinterpret_cast<HANDLE>(process_handle_), &code) == 0) {
    return Error(ErrorCode::INTERNAL_ERROR, "GetExitCodeProcess failed");
  }
  return static_cast<int>(code);
}

bool ChildProcess::running() const {
  if (process_handle_ == 0) {
    return false;
  }
  return ::WaitForSingleObject(reinterpret_cast<HANDLE>(process_handle_), 0) == WAIT_TIMEOUT;
}

Status ChildProcess::kill() {
  if (process_handle_ == 0) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "the process handle is closed");
  }
  if (::TerminateProcess(reinterpret_cast<HANDLE>(process_handle_), 1) == 0) {
    return Error(ErrorCode::INTERNAL_ERROR, "TerminateProcess failed",
                 std::to_string(::GetLastError()));
  }
  return Status{};
}

std::vector<std::string> ChildProcess::transcript() const {
  if (!output_) {
    return {};
  }
  std::shared_ptr<OutputState> state = output_;
  std::lock_guard<std::mutex> guard(state->mutex);
  return state->transcript;
}

#else  // POSIX fallback

ChildProcess::~ChildProcess() = default;

void ChildProcess::start_reader() {}

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const ProcessOptions& options) {
  return make_error(ErrorCode::NOT_SUPPORTED,
                    "child process management is implemented for Windows in this release");
}

Result<std::string> ChildProcess::wait_for_line(
    const std::function<bool(const std::string&)>&) {
  return make_error(ErrorCode::NOT_SUPPORTED, "unavailable");
}

bool ChildProcess::try_read_line(std::string*) { return false; }
void ChildProcess::wait_for_output_end() {}
Result<int> ChildProcess::wait() {
  return make_error(ErrorCode::NOT_SUPPORTED, "unavailable");
}
bool ChildProcess::running() const { return false; }
Status ChildProcess::kill() {
  return make_error(ErrorCode::NOT_SUPPORTED, "unavailable");
}
std::vector<std::string> ChildProcess::transcript() const { return {}; }

#endif

}  // namespace ccsf
