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

// The listening endpoint of the ihs-vk-export protocol. The Server owns the
// AF_UNIX/SOCK_SEQPACKET listen socket, accepts one consumer at a time, and
// runs a Session per accepted connection on its own IO thread. When a consumer
// disconnects the loop returns to accept(), so reconnect is just the next
// accept. Start() spins the thread up; Stop() (also run from ~Server) breaks
// both the accept poll and any in-flight Session and joins. PresentFrame()
// forwards to the live Session (if any) and is safe to call from the shell's
// raster thread.

#ifndef IHS_CARLA_BRIDGE_SERVER_H_
#define IHS_CARLA_BRIDGE_SERVER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace ihs_vke {

class ExportSource;
class Session;

class Server {
 public:
  // `socket_path` is the AF_UNIX path to bind/listen on; `source` outlives the
  // Server.
  Server(std::string socket_path, ExportSource& source);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Create/bind/listen the socket and spawn the accept thread. Returns false
  // (after cleaning up) on any setup failure.
  bool Start();

  // Forward a present to the currently connected consumer's Session, if any.
  // THREAD-SAFE: called from the shell's raster-thread frame listener.
  void PresentFrame(uint32_t slot, uint32_t seq);

  // Stop accepting, tear down any in-flight Session, join the thread, and
  // unlink the socket path. Idempotent; also called by the destructor.
  void Stop();

 private:
  // The IO thread: accept -> serve one Session -> repeat until stopped.
  void AcceptLoop();

  std::string socket_path_;
  ExportSource& source_;

  int listen_fd_ = -1;
  int accept_wake_fd_ = -1;  // eventfd (EFD_CLOEXEC) to break the accept poll

  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool started_ = false;

  std::mutex session_mutex_;
  Session* current_session_ = nullptr;  // guarded by session_mutex_
};

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_SERVER_H_
