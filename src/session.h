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

// One consumer session of the ihs-vk-export protocol: it drives a single
// connected AF_UNIX/SOCK_SEQPACKET consumer socket from an abstract
// ExportSource. The Session owns neither the socket (the caller closes it) nor
// the source. Handshake() runs the HELLO/CAPS/IMAGE_TABLE exchange; Run() is
// the blocking event loop (socket + an internal eventfd wakeup); PresentFrame()
// and Stop() are the two entry points safe to call from other threads.

#ifndef IHS_CARLA_BRIDGE_SESSION_H_
#define IHS_CARLA_BRIDGE_SESSION_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace ihs_vke {

class ExportSource;

class Session {
 public:
  // `sock` is a connected AF_UNIX SOCK_SEQPACKET fd, NOT owned by the Session
  // (the caller closes it). `source` outlives the Session.
  Session(int sock, ExportSource& source);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // HELLO/CAPS/IMAGE_TABLE handshake. Returns false (after best-effort Bye) on
  // any protocol/device mismatch. Call once, before Run().
  bool Handshake();

  // Blocking event loop: services the socket and the outbound present queue
  // until the peer goes away, sends Bye, or Stop() is called.
  void Run();

  // Enqueue a FramePresent for `slot`/`frame_seq` and wake Run(). THREAD-SAFE:
  // called from the shell's raster-thread frame listener.
  void PresentFrame(uint32_t slot, uint32_t frame_seq);

  // Ask Run() to return at the next loop turn. THREAD-SAFE.
  void Stop();

 private:
  struct FramePresentItem {
    uint32_t slot;
    uint32_t frame_seq;
  };

  // Fill + send a kImageTable at `generation`, then close every fd the source
  // dup'd into it. Records `generation` as the current generation.
  void SendImageTable(uint32_t generation);

  // Drain the outbound present queue, sending a FramePresent for each item.
  void DrainPresentQueue();

  // Nudge the eventfd so a blocked poll() in Run() wakes. Thread-safe.
  void WakeEventFd();

  int sock_;  // not owned
  ExportSource& source_;
  int event_fd_ = -1;  // owned; EFD_CLOEXEC wakeup for Run()

  std::atomic<bool> stop_{false};

  std::mutex queue_mutex_;
  std::deque<FramePresentItem> present_queue_;

  uint32_t generation_ = 0;

  // Current pool extent, cached for FramePresent's (informational) full-damage
  // rect. Set at Handshake from caps(); updated on a successful resize.
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_SESSION_H_
