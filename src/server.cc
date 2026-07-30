/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "server.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "export_source.h"
#include "session.h"

namespace ihs_vke {

Server::Server(std::string socket_path, ExportSource& source)
    : socket_path_(std::move(socket_path)), source_(source) {}

Server::~Server() { Stop(); }

bool Server::Start() {
  if (started_) {
    return false;
  }

  if (socket_path_.size() >= sizeof(sockaddr_un{}.sun_path)) {
    std::fprintf(stderr, "Server: socket path too long: %s\n",
                 socket_path_.c_str());
    return false;
  }

  listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    std::fprintf(stderr, "Server: socket() failed: %s\n", std::strerror(errno));
    return false;
  }

  // A stale socket file from a prior run would make bind() fail with
  // EADDRINUSE.
  if (::unlink(socket_path_.c_str()) != 0 && errno != ENOENT) {
    std::fprintf(stderr, "Server: unlink(%s) failed: %s\n",
                 socket_path_.c_str(), std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size());

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    std::fprintf(stderr, "Server: bind(%s) failed: %s\n", socket_path_.c_str(),
                 std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (::listen(listen_fd_, 1) != 0) {
    std::fprintf(stderr, "Server: listen() failed: %s\n", std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(socket_path_.c_str());
    return false;
  }

  accept_wake_fd_ = ::eventfd(0, EFD_CLOEXEC);
  if (accept_wake_fd_ < 0) {
    std::fprintf(stderr, "Server: eventfd() failed: %s\n",
                 std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(socket_path_.c_str());
    return false;
  }

  stop_.store(false, std::memory_order_release);
  started_ = true;
  thread_ = std::thread(&Server::AcceptLoop, this);
  return true;
}

void Server::AcceptLoop() {
  while (!stop_.load(std::memory_order_acquire)) {
    pollfd pfds[2];
    pfds[0].fd = listen_fd_;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = accept_wake_fd_;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    int n = ::poll(pfds, 2, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // unrecoverable poll error.
    }

    // Stop requested: drain the eventfd and exit the loop.
    if ((pfds[1].revents & POLLIN) != 0) {
      uint64_t counter = 0;
      ssize_t r;
      do {
        r = ::read(accept_wake_fd_, &counter, sizeof(counter));
      } while (r < 0 && errno == EINTR);
      break;
    }

    if ((pfds[0].revents & POLLIN) == 0) {
      continue;  // spurious / error revents on listen fd handled by accept.
    }

    int fd;
    do {
      fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;  // unrecoverable accept error.
    }

    // Serve exactly this one consumer to completion; reconnect is the next
    // accept. current_session_ points at this stack Session and is only ever
    // touched under session_mutex_, so PresentFrame()/Stop() see it as
    // valid-or-null.
    Session session(fd, source_);
    {
      std::lock_guard<std::mutex> lk(session_mutex_);
      current_session_ = &session;
    }
    if (session.Handshake()) {
      source_.on_consumer_attached();
      session.Run();
      source_.on_consumer_detached();
    }
    {
      std::lock_guard<std::mutex> lk(session_mutex_);
      current_session_ = nullptr;
    }
    ::close(fd);
  }
}

void Server::PresentFrame(uint32_t slot, uint32_t seq) {
  std::lock_guard<std::mutex> lk(session_mutex_);
  if (current_session_ != nullptr) {
    current_session_->PresentFrame(slot, seq);
  }
}

void Server::Stop() {
  if (!started_) {
    return;
  }

  stop_.store(true, std::memory_order_release);

  // Break a blocked accept poll.
  if (accept_wake_fd_ >= 0) {
    const uint64_t one = 1;
    ssize_t w;
    do {
      w = ::write(accept_wake_fd_, &one, sizeof(one));
    } while (w < 0 && errno == EINTR);
  }

  // Break an in-flight Session::Run().
  {
    std::lock_guard<std::mutex> lk(session_mutex_);
    if (current_session_ != nullptr) {
      current_session_->Stop();
    }
  }

  if (thread_.joinable()) {
    thread_.join();
  }

  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_wake_fd_ >= 0) {
    ::close(accept_wake_fd_);
    accept_wake_fd_ = -1;
  }
  ::unlink(socket_path_.c_str());
  started_ = false;
}

}  // namespace ihs_vke
