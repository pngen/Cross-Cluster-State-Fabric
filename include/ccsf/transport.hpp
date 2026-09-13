// Cross-Cluster State Fabric - framed TCP transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The transport is a thin, bounded, versioned frame pipe. It assumes nothing
// about message boundaries: every read and write loops until the requested
// number of bytes has crossed the socket, and a peer that closes mid-frame is
// reported as a truncated frame rather than being silently accepted.

#ifndef CCSF_TRANSPORT_HPP
#define CCSF_TRANSPORT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "ccsf/protocol.hpp"
#include "ccsf/result.hpp"

namespace ccsf {

/// Process-lifetime socket subsystem handle. On platforms without Winsock this
/// is a no-op. Constructing or destroying one never invalidates a live socket:
/// initialization happens once per process and teardown happens at exit.
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;
};

/// A connected, framed byte pipe.
class TcpConnection {
 public:
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;

  /// Connects to a host:port. "127.0.0.1" is used for the reference deployment.
  static Result<std::unique_ptr<TcpConnection>> connect(const std::string& host,
                                                        std::uint16_t port);

  /// Sends a complete frame. Loops until every byte has been handed to the
  /// socket; never assumes one send equals one receive.
  Status send_frame(const Frame& frame);

  /// Receives one complete frame. Returns FRAME_TRUNCATED when the peer closes
  /// in the middle of a frame and TRANSPORT_CLOSED when it closes cleanly
  /// between frames.
  Result<Frame> receive_frame(std::size_t max_payload = kMaxFramePayloadBytes);

  void close();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string peer() const;

  /// Raw socket handle wrapper used by the listener.
  explicit TcpConnection(std::uintptr_t handle, std::string peer);

 private:
  [[nodiscard]] std::uintptr_t handle() const noexcept { return handle_; }
  friend class TcpListener;

  std::uintptr_t handle_{0};
  std::string peer_;
  std::uint64_t next_sequence_{1};
};

/// A bound listening socket.
class TcpListener {
 public:
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  /// Binds and listens on the given address. Port 0 selects an ephemeral port.
  static Result<std::unique_ptr<TcpListener>> bind(const std::string& address,
                                                   std::uint16_t port,
                                                   int backlog = 16);

  /// Blocks until a peer connects. Returns TRANSPORT_CLOSED once the listener
  /// has been closed, so a shutdown loop terminates naturally.
  Result<std::unique_ptr<TcpConnection>> accept();

  void close();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != 0; }

 private:
  TcpListener(std::uintptr_t handle, std::uint16_t port) : handle_(handle), port_(port) {}

  std::uintptr_t handle_{0};
  std::uint16_t port_{0};
};

/// Convenience: a loopback address for single-host reference deployments.
inline const char* loopback_address() noexcept { return "127.0.0.1"; }

}  // namespace ccsf

#endif  // CCSF_TRANSPORT_HPP
