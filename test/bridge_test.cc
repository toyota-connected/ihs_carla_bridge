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

// Whole-module test: drives the Bridge (ShellExportSource + Server + Session +
// wire) against a FAKE shell ABI defined in this file, injected via the
// Bridge's get_api hook (never dlsym). It proves ShellExportSource's delegation
// (caps / image table / release / inject / pace source), the frame-listener
// thunk wiring (a raster-thread present reaches the consumer), and consumer
// attach/detach pacing -- end to end over a real AF_UNIX/SOCK_SEQPACKET socket.
// Mirrors server_test's Check() style; no gtest.

#include "bridge.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "ihs_carla_bridge/vk_export.h"
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

// ---- Fake shell ABI state (mirrors what the headless-vulkan backend records)
struct FakeState {
  std::atomic<int> release_count{0};
  std::atomic<int> last_release_slot{-1};
  std::atomic<int> inject_count{0};
  std::atomic<uint32_t> last_pointer_phase{0xffffffffu};
  std::atomic<int> resize_count{0};
  std::atomic<uint32_t> last_pace_source{0xffffffffu};

  std::atomic<IhsVkFrameListener> listener_cb{nullptr};
  std::atomic<void*> listener_user{nullptr};
};
FakeState g_fake;

// Real, readable fd: read end of a fresh pipe with a marker byte queued. The
// Session closes each dup'd fd after sending the image table.
int MakeFd() {
  int p[2];
  if (::pipe(p) != 0) {
    return -1;
  }
  const char mark = 'X';
  (void)!::write(p[1], &mark, 1);
  ::close(p[1]);
  return p[0];
}

IhsVkExportCaps FakeGetCaps() {
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

int FakeGetImageTable(IhsVkExportImageTable* out) {
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
  return 0;
}

void FakeSetFrameListener(IhsVkFrameListener cb, void* user) {
  g_fake.listener_cb.store(cb);
  g_fake.listener_user.store(user);
}

void FakeReleaseFrame(uint32_t slot) {
  g_fake.last_release_slot.store(static_cast<int>(slot));
  ++g_fake.release_count;
}

void FakeSetPaceSource(uint32_t src) { g_fake.last_pace_source.store(src); }

void FakeInjectPointer(const IhsPointerEvent* ev) {
  g_fake.last_pointer_phase.store(ev->phase);
  ++g_fake.inject_count;
}

int FakeRebuildPool(uint32_t, uint32_t) {
  ++g_fake.resize_count;
  return 0;
}

const IhsVkExportApiV1 kApi = {
    /*get_caps=*/&FakeGetCaps,
    /*get_image_table=*/&FakeGetImageTable,
    /*set_frame_listener=*/&FakeSetFrameListener,
    /*release_frame=*/&FakeReleaseFrame,
    /*set_pace_source=*/&FakeSetPaceSource,
    /*inject_pointer=*/&FakeInjectPointer,
    /*rebuild_pool=*/&FakeRebuildPool,
};

const IhsVkExportApiV1* fake_get_api(uint32_t version) {
  return version == IHS_VK_EXPORT_API_VERSION ? &kApi : nullptr;
}

// ---- Consumer helpers (mirror server_test) ---------------------------------
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

bool ConsumerHandshake(int sock) {
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
      "/tmp/ihs_vke_bridge_" + std::to_string(::getpid()) + ".sock";

  Bridge bridge(path, fake_get_api);
  Check(bridge.valid(), "bridge valid (fake api bound)");
  Check(bridge.Start(), "bridge start");

  // The frame listener must have been registered during Start().
  Check(g_fake.listener_cb.load() != nullptr, "frame listener registered");
  Check(g_fake.listener_user.load() == static_cast<void*>(&bridge),
        "frame listener user == bridge");

  int c = ConnectConsumer(path);
  Check(c >= 0, "consumer connect");
  if (c >= 0) {
    Check(ConsumerHandshake(c), "consumer handshake");

    // on_consumer_attached -> set_pace_source(CONSUMER_DRIVEN).
    Check(SpinWait(
              [&] {
                return g_fake.last_pace_source.load() ==
                       IHS_VK_PACE_CONSUMER_DRIVEN;
              },
              2000),
          "pace source consumer-driven on attach");

    // Simulate the shell raster thread presenting a frame: invoke the stored
    // listener directly. FrameThunk -> Server::PresentFrame -> the consumer.
    IhsVkFrameListener cb = g_fake.listener_cb.load();
    void* user = g_fake.listener_user.load();
    Check(cb != nullptr, "listener present");
    if (cb != nullptr) {
      cb(/*slot=*/1, /*frame_seq=*/5, user);
    }

    RecvMsg present_msg = RecvFramed(c);
    Check(present_msg.ok && present_msg.type == MsgType::kFramePresent,
          "recv FramePresent");
    if (present_msg.payload.size() == sizeof(FramePresent)) {
      FramePresent fp{};
      std::memcpy(&fp, present_msg.payload.data(), sizeof(fp));
      Check(fp.slot == 1 && fp.frame_seq == 5, "FramePresent slot/seq");
    } else {
      Check(false, "FramePresent size");
    }
    CloseFds(present_msg);

    // Consumer releases the slot -> ShellExportSource::on_frame_release ->
    // fake release_frame.
    FrameRelease fr{/*slot=*/1, /*generation=*/0};
    Check(SendPod(c, MsgType::kFrameRelease, fr), "send FrameRelease");
    Check(SpinWait([&] { return g_fake.release_count.load() == 1; }, 2000),
          "release_frame fired");
    Check(g_fake.last_release_slot.load() == 1, "release_frame saw slot 1");

    // Consumer injects a pointer -> ShellExportSource::on_input_pointer ->
    // fake inject_pointer.
    InputPointer ip{};
    ip.phase = static_cast<uint32_t>(PointerPhase::kDown);
    ip.x = 12.5f;
    ip.y = 34.0f;
    ip.buttons = 1;
    ip.device_id = 7;
    Check(SendPod(c, MsgType::kInputPointer, ip), "send InputPointer");
    Check(SpinWait([&] { return g_fake.inject_count.load() == 1; }, 2000),
          "inject_pointer fired");
    Check(g_fake.last_pointer_phase.load() ==
              static_cast<uint32_t>(PointerPhase::kDown),
          "inject_pointer saw phase down");

    ::close(c);
  }

  // on_consumer_detached -> set_pace_source(FREE_RUN).
  Check(SpinWait(
            [&] {
              return g_fake.last_pace_source.load() == IHS_VK_PACE_FREE_RUN;
            },
            2000),
        "pace source free-run on detach");

  bridge.Stop();
  Check(::access(path.c_str(), F_OK) != 0, "socket path unlinked after Stop");

  if (g_failures == 0) {
    std::printf("bridge_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "bridge_test: %d failure(s)\n", g_failures);
  return 1;
}
