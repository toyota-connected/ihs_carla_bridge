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

#include "session.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "export_source.h"
#include "ihs_carla_bridge/wire_protocol.h"
#include "scm_rights.h"

namespace ihs_vke {

namespace {

// Map the shell C ABI handle-type enum (IHS_VK_HANDLE_*) to a wire
// HandleTypeBits value.
uint32_t MapHandleType(uint32_t ihs_handle_type) {
  switch (ihs_handle_type) {
    case IHS_VK_HANDLE_OPAQUE_FD:
      return kHandleOpaqueFd;
    case IHS_VK_HANDLE_DMA_BUF:
      return kHandleDmaBuf;
    default:
      return 0;
  }
}

// Translate one shell-ABI image descriptor to its wire form (fds excluded --
// those ride the datagram as SCM_RIGHTS).
ImageDesc ToWireDesc(const IhsVkExportImage& img) {
  ImageDesc desc{};
  desc.alloc_size = img.alloc_size;
  desc.drm_modifier = img.drm_modifier;
  for (uint32_t p = 0; p < kMaxPlanes && p < IHS_VK_EXPORT_MAX_PLANES; ++p) {
    desc.plane_offset[p] = img.plane_offset[p];
    desc.plane_pitch[p] = img.plane_pitch[p];
  }
  desc.width = img.width;
  desc.height = img.height;
  desc.vk_format = img.vk_format;
  desc.drm_fourcc = img.drm_fourcc;
  desc.memory_type_index = img.memory_type_index;
  desc.handle_type = MapHandleType(img.handle_type);
  desc.plane_count = img.plane_count;
  desc.reserved = 0;
  return desc;
}

}  // namespace

Session::Session(int sock, ExportSource& source)
    : sock_(sock), source_(source) {
  event_fd_ = ::eventfd(0, EFD_CLOEXEC);
}

Session::~Session() {
  if (event_fd_ >= 0) {
    ::close(event_fd_);
    event_fd_ = -1;
  }
}

void Session::WakeEventFd() {
  const uint64_t one = 1;
  ssize_t n;
  do {
    n = ::write(event_fd_, &one, sizeof(one));
  } while (n < 0 && errno == EINTR);
}

bool Session::Handshake() {
  RecvMsg m = RecvFramed(sock_);
  if (!m.ok || m.type != MsgType::kHello || m.payload.size() < sizeof(Hello)) {
    CloseFds(m);
    return false;
  }
  Hello hello{};
  std::memcpy(&hello, m.payload.data(), sizeof(hello));
  CloseFds(m);  // a Hello carries no fds, but stay leak-proof.

  if (hello.protocol_version != kProtocolVersion) {
    Bye bye{static_cast<uint32_t>(ByeReason::kProtocolError)};
    SendPod(sock_, MsgType::kBye, bye);
    return false;
  }

  const IhsVkExportCaps c = source_.caps();
  if (std::memcmp(hello.device_uuid, c.device_uuid, 16) != 0) {
    Bye bye{static_cast<uint32_t>(ByeReason::kDeviceMismatch)};
    SendPod(sock_, MsgType::kBye, bye);
    return false;
  }

  width_ = c.width;
  height_ = c.height;

  Caps caps{};
  caps.protocol_version = kProtocolVersion;
  caps.handle_types = MapHandleType(c.handle_type);
  caps.vk_format = c.vk_format;
  caps.drm_fourcc = c.drm_fourcc;
  caps.width = c.width;
  caps.height = c.height;
  caps.slot_count = c.slot_count;
  caps.flags = 0;
  std::memcpy(caps.device_uuid, c.device_uuid, 16);
  if (!SendPod(sock_, MsgType::kCaps, caps)) {
    return false;
  }

  SendImageTable(generation_);  // initial table at generation 0
  return true;
}

void Session::SendImageTable(uint32_t generation) {
  generation_ = generation;

  IhsVkExportImageTable table{};
  if (!source_.fill_image_table(&table)) {
    return;  // no exportable pool; nothing to advertise.
  }

  const uint32_t slot_count = table.slot_count;

  std::vector<uint8_t> payload(sizeof(ImageTableHeader) +
                               static_cast<size_t>(slot_count) *
                                   sizeof(ImageDesc));
  ImageTableHeader hdr{generation, slot_count};
  std::memcpy(payload.data(), &hdr, sizeof(hdr));

  // Assemble the SCM_RIGHTS fds in slot order:
  //   [ mem_0, render_done_0, consumer_done_0, mem_1, ... ]. We assume an
  // exportable pool provides all three fds per slot (so a full table carries
  // 3 * slot_count fds); a slot is skipped only if all three are absent, and
  // otherwise only the valid (>= 0) fds are forwarded, in order.
  std::vector<int> fds;
  fds.reserve(ImageTableFdCount(slot_count));
  for (uint32_t s = 0; s < slot_count; ++s) {
    const IhsVkExportImage& img = table.images[s];
    const ImageDesc desc = ToWireDesc(img);
    std::memcpy(
        payload.data() + sizeof(ImageTableHeader) + s * sizeof(ImageDesc),
        &desc, sizeof(desc));

    const bool none =
        img.memory_fd < 0 && img.render_done_fd < 0 && img.consumer_done_fd < 0;
    if (none) {
      continue;
    }
    if (img.memory_fd >= 0) {
      fds.push_back(img.memory_fd);
    }
    if (img.render_done_fd >= 0) {
      fds.push_back(img.render_done_fd);
    }
    if (img.consumer_done_fd >= 0) {
      fds.push_back(img.consumer_done_fd);
    }
  }

  SendFramed(sock_, MsgType::kImageTable, payload.data(), payload.size(),
             fds.data(), fds.size());

  // The consumer received its own dup'd copies via SCM_RIGHTS; close every fd
  // the source dup'd into the table and null it out.
  for (uint32_t s = 0; s < slot_count; ++s) {
    IhsVkExportImage& img = table.images[s];
    if (img.memory_fd >= 0) {
      ::close(img.memory_fd);
      img.memory_fd = -1;
    }
    if (img.render_done_fd >= 0) {
      ::close(img.render_done_fd);
      img.render_done_fd = -1;
    }
    if (img.consumer_done_fd >= 0) {
      ::close(img.consumer_done_fd);
      img.consumer_done_fd = -1;
    }
  }
}

void Session::DrainPresentQueue() {
  std::deque<FramePresentItem> local;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    local.swap(present_queue_);
  }
  for (const FramePresentItem& item : local) {
    FramePresent fp{};
    fp.slot = item.slot;
    fp.frame_seq = item.frame_seq;
    fp.generation = generation_;
    fp.damage_x = 0;
    fp.damage_y = 0;
    fp.damage_w = static_cast<int32_t>(width_);
    fp.damage_h = static_cast<int32_t>(height_);
    SendPod(sock_, MsgType::kFramePresent, fp);
  }
}

void Session::Run() {
  pollfd pfds[2];
  pfds[0].fd = sock_;
  pfds[0].events = POLLIN;
  pfds[1].fd = event_fd_;
  pfds[1].events = POLLIN;

  while (!stop_.load(std::memory_order_acquire)) {
    pfds[0].revents = 0;
    pfds[1].revents = 0;

    int n = ::poll(pfds, 2, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // unrecoverable poll error.
    }

    // A hung-up or errored socket ends the session.
    if ((pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      break;
    }

    // Wakeup: drain the eventfd counter, then flush pending presents.
    if ((pfds[1].revents & POLLIN) != 0) {
      uint64_t counter = 0;
      ssize_t r;
      do {
        r = ::read(event_fd_, &counter, sizeof(counter));
      } while (r < 0 && errno == EINTR);
      if (stop_.load(std::memory_order_acquire)) {
        break;
      }
      DrainPresentQueue();
    }

    // Inbound consumer message.
    if ((pfds[0].revents & POLLIN) != 0) {
      RecvMsg m = RecvFramed(sock_);
      if (m.closed || !m.ok) {
        CloseFds(m);
        break;  // peer closed, truncated, or errored.
      }

      bool done = false;
      switch (m.type) {
        case MsgType::kFrameRelease: {
          if (m.payload.size() >= sizeof(FrameRelease)) {
            FrameRelease fr{};
            std::memcpy(&fr, m.payload.data(), sizeof(fr));
            source_.on_frame_release(fr.slot);
          }
          break;
        }
        case MsgType::kInputPointer: {
          if (m.payload.size() >= sizeof(InputPointer)) {
            InputPointer ip{};
            std::memcpy(&ip, m.payload.data(), sizeof(ip));
            IhsPointerEvent ev{};
            ev.phase = ip.phase;
            ev.x = ip.x;
            ev.y = ip.y;
            ev.buttons = ip.buttons;
            ev.device_id = ip.device_id;
            source_.on_input_pointer(ev);
          }
          break;
        }
        case MsgType::kResize: {
          if (m.payload.size() >= sizeof(Resize)) {
            Resize rz{};
            std::memcpy(&rz, m.payload.data(), sizeof(rz));
            if (source_.on_resize(rz.width, rz.height)) {
              width_ = rz.width;
              height_ = rz.height;
              SendImageTable(++generation_);
            }
          }
          break;
        }
        case MsgType::kBye:
          done = true;
          break;
        case MsgType::kInputKey:
          break;  // reserved for v1; ignore.
        default:
          break;  // ignore unexpected B->C or unknown types.
      }

      CloseFds(m);
      if (done) {
        break;
      }
    }
  }
}

void Session::PresentFrame(uint32_t slot, uint32_t frame_seq) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    present_queue_.push_back(FramePresentItem{slot, frame_seq});
  }
  WakeEventFd();
}

void Session::Stop() {
  stop_.store(true, std::memory_order_release);
  WakeEventFd();
}

}  // namespace ihs_vke
