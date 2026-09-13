// Cross-Cluster State Fabric - framed TCP transport implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/transport.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ccsf/crc32c.hpp"

namespace ccsf {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

int close_socket(SocketHandle handle) { return ::closesocket(handle); }
int last_socket_error() { return ::WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT; }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

int close_socket(SocketHandle handle) { return ::close(handle); }
int last_socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
#endif

Error transport_error(ErrorCode code, const std::string& message) {
  return Error(code, message, "socket error " + std::to_string(last_socket_error()));
}

/// Process-lifetime socket subsystem. Initialization happens exactly once and
/// teardown happens at process exit, so a socket can never be invalidated by a
/// temporary lifetime object going out of scope while it is still in use.
struct SocketLifetime {
  SocketLifetime() {
#if defined(_WIN32)
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      started = false;
    }
#endif
  }
  ~SocketLifetime() {
#if defined(_WIN32)
    if (started) {
      ::WSACleanup();
    }
#endif
  }
  bool started{true};
};

void ensure_socket_runtime() {
  static SocketLifetime lifetime;
  (void)lifetime;
}

}  // namespace

SocketRuntime::SocketRuntime() { ensure_socket_runtime(); }

SocketRuntime::~SocketRuntime() = default;

TcpConnection::TcpConnection(std::uintptr_t handle, std::string peer)
    : handle_(handle), peer_(std::move(peer)) {}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : handle_(other.handle_), peer_(std::move(other.peer_)), next_sequence_(other.next_sequence_) {
  other.handle_ = 0;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    peer_ = std::move(other.peer_);
    next_sequence_ = other.next_sequence_;
    other.handle_ = 0;
  }
  return *this;
}

void TcpConnection::close() {
  if (handle_ != 0) {
    close_socket(static_cast<SocketHandle>(handle_));
    handle_ = 0;
  }
}

bool TcpConnection::valid() const noexcept { return handle_ != 0; }

std::string TcpConnection::peer() const { return peer_; }

Result<std::unique_ptr<TcpConnection>> TcpConnection::connect(const std::string& host,
                                                              std::uint16_t port) {
  ensure_socket_runtime();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Error(ErrorCode::TRANSPORT_IO, "address resolution failed", host + ":" + service);
  }
  SocketHandle handle = kInvalidSocket;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == kInvalidSocket) {
      continue;
    }
    if (::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_socket(handle);
    handle = kInvalidSocket;
  }
  ::freeaddrinfo(results);
  if (handle == kInvalidSocket) {
    return transport_error(ErrorCode::TRANSPORT_IO, "connect failed");
  }
  int flag = 1;
  ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
               sizeof(flag));
  return std::unique_ptr<TcpConnection>(
      new TcpConnection(static_cast<std::uintptr_t>(handle), host + ":" + service));
}

Status TcpConnection::send_frame(const Frame& frame) {
  if (handle_ == 0) {
    return make_error(ErrorCode::TRANSPORT_CLOSED, "the connection is closed");
  }
  Frame outgoing = frame;
  if (outgoing.header.sequence == 0) {
    outgoing.header.sequence = next_sequence_++;
  }
  const std::vector<std::byte> image = encode_frame(outgoing);
  std::size_t offset = 0;
  while (offset < image.size()) {
    const std::size_t remaining = image.size() - offset;
    const int chunk = remaining > (1U << 20U) ? static_cast<int>(1U << 20U)
                                              : static_cast<int>(remaining);
    const int written = ::send(static_cast<SocketHandle>(handle_),
                               reinterpret_cast<const char*>(image.data() + offset), chunk, 0);
    if (written <= 0) {
      return transport_error(ErrorCode::TRANSPORT_IO, "send failed");
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status{};
}

Result<Frame> TcpConnection::receive_frame(std::size_t max_payload) {
  if (handle_ == 0) {
    return make_error(ErrorCode::TRANSPORT_CLOSED, "the connection is closed");
  }
  std::array<std::byte, kFrameHeaderBytes> header_bytes{};
  std::size_t received = 0;
  while (received < header_bytes.size()) {
    const int chunk = static_cast<int>(header_bytes.size() - received);
    const int read = ::recv(static_cast<SocketHandle>(handle_),
                            reinterpret_cast<char*>(header_bytes.data() + received), chunk, 0);
    if (read == 0) {
      if (received == 0) {
        return make_error(ErrorCode::TRANSPORT_CLOSED, "the peer closed the connection");
      }
      return make_error(ErrorCode::FRAME_TRUNCATED, "the peer closed during a frame header");
    }
    if (read < 0) {
      const int error = last_socket_error();
      if (would_block(error)) {
        continue;
      }
      return transport_error(ErrorCode::TRANSPORT_IO, "receive failed");
    }
    received += static_cast<std::size_t>(read);
  }
  auto header = decode_frame_header(ByteSpan(header_bytes.data(), header_bytes.size()));
  if (!header) {
    return header.error();
  }
  if (header.value().payload_length > max_payload) {
    return Error(ErrorCode::FRAME_TOO_LARGE, "the declared payload exceeds the receive bound");
  }
  Frame frame;
  frame.header = header.value();
  frame.payload.resize(frame.header.payload_length);
  std::size_t payload_received = 0;
  while (payload_received < frame.payload.size()) {
    const int chunk = static_cast<int>(frame.payload.size() - payload_received);
    const int read = ::recv(static_cast<SocketHandle>(handle_),
                            reinterpret_cast<char*>(frame.payload.data() + payload_received), chunk,
                            0);
    if (read == 0) {
      return make_error(ErrorCode::FRAME_TRUNCATED, "the peer closed during a frame payload");
    }
    if (read < 0) {
      const int error = last_socket_error();
      if (would_block(error)) {
        continue;
      }
      return transport_error(ErrorCode::TRANSPORT_IO, "receive failed");
    }
    payload_received += static_cast<std::size_t>(read);
  }
  if (!frame.payload.empty() &&
      crc32c(ByteSpan(frame.payload.data(), frame.payload.size())) != frame.header.payload_crc) {
    return Error(ErrorCode::FRAME_CORRUPT, "frame payload checksum mismatch");
  }
  return frame;
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
  if (handle_ != 0) {
    close_socket(static_cast<SocketHandle>(handle_));
    handle_ = 0;
  }
}

Result<std::unique_ptr<TcpListener>> TcpListener::bind(const std::string& address,
                                                       std::uint16_t port, int backlog) {
  ensure_socket_runtime();
  SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidSocket) {
    return transport_error(ErrorCode::TRANSPORT_IO, "socket creation failed");
  }
  int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    close_socket(handle);
    return Error(ErrorCode::INVALID_ARGUMENT, "invalid bind address", address);
  }
  if (::bind(handle, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    close_socket(handle);
    return transport_error(ErrorCode::TRANSPORT_IO, "bind failed");
  }
  if (::listen(handle, backlog) != 0) {
    close_socket(handle);
    return transport_error(ErrorCode::TRANSPORT_IO, "listen failed");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    close_socket(handle);
    return transport_error(ErrorCode::TRANSPORT_IO, "getsockname failed");
  }
  return std::unique_ptr<TcpListener>(
      new TcpListener(static_cast<std::uintptr_t>(handle), ntohs(bound.sin_port)));
}

Result<std::unique_ptr<TcpConnection>> TcpListener::accept() {
  if (handle_ == 0) {
    return make_error(ErrorCode::TRANSPORT_CLOSED, "the listener is closed");
  }
  sockaddr_in remote{};
#if defined(_WIN32)
  int remote_length = sizeof(remote);
#else
  socklen_t remote_length = sizeof(remote);
#endif
  const SocketHandle accepted = ::accept(static_cast<SocketHandle>(handle_),
                                         reinterpret_cast<sockaddr*>(&remote), &remote_length);
  if (accepted == kInvalidSocket) {
    return make_error(ErrorCode::TRANSPORT_CLOSED, "the listener stopped accepting");
  }
  int flag = 1;
  ::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
               sizeof(flag));
  char text[64] = {};
  const bool resolved = ::inet_ntop(AF_INET, &remote.sin_addr, text, sizeof(text)) != nullptr;
  std::string peer = std::string(resolved ? text : "unknown") + ":" +
                     std::to_string(ntohs(remote.sin_port));
  return std::unique_ptr<TcpConnection>(
      new TcpConnection(static_cast<std::uintptr_t>(accepted), std::move(peer)));
}

}  // namespace ccsf
