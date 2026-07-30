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

// End-to-end Server test over a real AF_UNIX/SOCK_SEQPACKET listen socket. A
// MockSource (backed by real dup'd pipe fds, like session_test) stands in for
// the shell C ABI. Exercises the full lifecycle: consumer #1 connects, gets
// caps + image table, is presented a frame and releases it, then disconnects;
// consumer #2 reconnects and re-handshakes (proving the accept loop reuses the
// same listen socket); finally Stop() unlinks the socket path. Mirrors
// session_test's Check() style; no gtest.

#include "server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "export_source.h"
#include "ihs_carla_bridge/wire_protocol.h"
#include "scm_rights.h"

using namespace ihs_vke;

namespace {
int g_failures = 0;
void Check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

const uint8_t kUuid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                           0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01};
constexpr uint32_t kSlots = 2;
constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 480;
constexpr uint32_t kVkFormat = 44;  // VK_FORMAT_R8G8B8A8_UNORM, arbitrary here.

class MockSource : public ExportSource {
 public:
  IhsVkExportCaps caps() override {
    IhsVkExportCaps c{};
    c.api_version = IHS_VK_EXPORT_API_VERSION;
    c.handle_type = IHS_VK_HANDLE_OPAQUE_FD;
    c.vk_format = kVkFormat;
    c.drm_fourcc = 0;
    c.width = kWidth;
    c.height = kHeight;
    c.slot_count = kSlots;
    c.export_active = 1;
    std::memcpy(c.device_uuid, kUuid, 16);
    return c;
  }

  bool fill_image_table(IhsVkExportImageTable* out) override {
    out->generation = 0;
    out->slot_count = kSlots;
    for (uint32_t s = 0; s < kSlots; ++s) {
      IhsVkExportImage& img = out->images[s];
      img.memory_fd = MakeFd();
      img.render_done_fd = MakeFd();
      img.consumer_done_fd = MakeFd();
      img.alloc_size = 0x100000 + s;
      img.drm_modifier = 0;
      img.width = kWidth;
      img.height = kHeight;
      img.vk_format = kVkFormat;
      img.drm_fourcc = 0;
      img.memory_type_index = 0;
      img.handle_type = IHS_VK_HANDLE_OPAQUE_FD;
      img.plane_count = 1;
    }
    ++fill_count;
    return true;
  }

  void on_frame_release(uint32_t slot) override {
    ++release_count;
    last_release_slot.store(static_cast<int>(slot));
  }
  void on_input_pointer(const IhsPointerEvent&) override {}
  bool on_resize(uint32_t, uint32_t) override { return true; }

  void on_consumer_attached() override { ++attached; }
  void on_consumer_detached() override { ++detached; }

  std::atomic<int> fill_count{0};
  std::atomic<int> release_count{0};
  std::atomic<int> attached{0};
  std::atomic<int> detached{0};
  std::atomic<int> last_release_slot{-1};

 private:
  // Real, readable fd: read end of a fresh pipe with a marker byte queued.
  static int MakeFd() {
    int p[2];
    if (::pipe(p) != 0) {
      return -1;
    }
    const char mark = 'X';
    (void)!::write(p[1], &mark, 1);
    ::close(p[1]);
    return p[0];  // caller (Session) closes this dup'd fd after sending.
  }
};

// Connect a fresh SEQPACKET consumer socket to `path`, retrying briefly in case
// the listen socket is not yet ready. Returns -1 on failure.
int ConnectConsumer(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ::close(fd);
  return -1;
}

bool SendHello(int sock, const uint8_t uuid[16], uint32_t version) {
  Hello h{};
  h.protocol_version = version;
  h.consumer_name_len = 0;
  std::memcpy(h.device_uuid, uuid, 16);
  return SendPod(sock, MsgType::kHello, h);
}

// Do the Hello -> Caps -> ImageTable handshake as a consumer, checking each
// step. Returns true if it all round-trips. `tag` labels failures.
bool ConsumerHandshake(int sock, const char* tag) {
  bool ok = true;
  auto note = [&](bool cond, const char* what) {
    Check(cond, what);
    ok = ok && cond;
  };

  note(SendHello(sock, kUuid, kProtocolVersion), "send Hello");

  RecvMsg caps_msg = RecvFramed(sock);
  note(caps_msg.ok && caps_msg.type == MsgType::kCaps, "recv Caps");
  if (caps_msg.payload.size() == sizeof(Caps)) {
    Caps caps{};
    std::memcpy(&caps, caps_msg.payload.data(), sizeof(caps));
    note(caps.slot_count == kSlots, "Caps slot_count");
    note(std::memcmp(caps.device_uuid, kUuid, 16) == 0, "Caps uuid");
  } else {
    note(false, "Caps size");
  }
  CloseFds(caps_msg);

  RecvMsg table_msg = RecvFramed(sock);
  note(table_msg.ok && table_msg.type == MsgType::kImageTable,
       "recv ImageTable");
  note(table_msg.fds.size() == ImageTableFdCount(kSlots),
       "ImageTable fd count == 6");
  for (int fd : table_msg.fds) {
    char c = 0;
    note(fd >= 0 && ::read(fd, &c, 1) == 1 && c == 'X', "table fd readable");
  }
  CloseFds(table_msg);
  (void)tag;
  return ok;
}

template <typename Pred>
bool SpinWait(Pred pred, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}
}  // namespace

int main() {
  const std::string path =
      "/tmp/ihs_vke_srv_" + std::to_string(::getpid()) + ".sock";

  MockSource mock;
  Server server(path, mock);
  Check(server.Start(), "start");

  // --- Consumer #1: full lifecycle -----------------------------------------
  int c1 = ConnectConsumer(path);
  Check(c1 >= 0, "consumer #1 connect");
  if (c1 >= 0) {
    Check(ConsumerHandshake(c1, "c1"), "consumer #1 handshake");

    server.PresentFrame(/*slot=*/1, /*seq=*/7);
    RecvMsg present_msg = RecvFramed(c1);
    Check(present_msg.ok && present_msg.type == MsgType::kFramePresent,
          "recv FramePresent");
    if (present_msg.payload.size() == sizeof(FramePresent)) {
      FramePresent fp{};
      std::memcpy(&fp, present_msg.payload.data(), sizeof(fp));
      Check(fp.slot == 1 && fp.frame_seq == 7, "FramePresent slot/seq");
    }
    CloseFds(present_msg);

    FrameRelease fr{/*slot=*/1, /*generation=*/0};
    Check(SendPod(c1, MsgType::kFrameRelease, fr), "send FrameRelease");

    // Let the Session read the FrameRelease before we close: closing races the
    // read (poll would report POLLIN|POLLHUP together and Run() honours the
    // hangup first), which would otherwise drop the release.
    Check(SpinWait([&] { return mock.release_count.load() == 1; }, 2000),
          "on_frame_release fired");

    ::close(c1);
  }

  Check(SpinWait([&] { return mock.detached.load() == 1; }, 2000),
        "consumer #1 detached");
  Check(mock.last_release_slot.load() == 1, "on_frame_release saw slot 1");

  // --- Consumer #2: reconnect ----------------------------------------------
  int c2 = ConnectConsumer(path);
  Check(c2 >= 0, "consumer #2 connect (reconnect)");
  if (c2 >= 0) {
    Check(ConsumerHandshake(c2, "c2"), "consumer #2 handshake (reconnect)");
    ::close(c2);
  }

  Check(SpinWait(
            [&] {
              return mock.attached.load() == 2 && mock.detached.load() == 2;
            },
            2000),
        "both consumers attached and detached");

  // --- Shutdown -------------------------------------------------------------
  server.Stop();
  Check(::access(path.c_str(), F_OK) != 0, "socket path unlinked after Stop");

  if (g_failures == 0) {
    std::printf("server_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "server_test: %d failure(s)\n", g_failures);
  return 1;
}
