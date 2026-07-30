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

// End-to-end Session test over a connected SEQPACKET socketpair (no
// listen/accept). A MockSource stands in for the shell C ABI; its image table
// is backed by REAL fds (dup'd from throwaway pipes) so the SCM_RIGHTS transfer
// hands the consumer live, readable descriptors. Exercises the happy path
// (handshake -> caps -> table -> present -> release -> bye) and the device
// mismatch rejection. Mirrors scm_rights_test's Check() style; no gtest.

#include "session.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
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
      img.alloc_size = 0x100000 + s;  // distinguishable per slot.
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
    last_release_slot = slot;
  }
  void on_input_pointer(const IhsPointerEvent& ev) override {
    ++pointer_count;
    last_pointer = ev;
  }
  bool on_resize(uint32_t width, uint32_t height) override {
    ++resize_count;
    last_resize_w = width;
    last_resize_h = height;
    return true;
  }

  int fill_count = 0;
  int release_count = 0;
  int pointer_count = 0;
  int resize_count = 0;
  uint32_t last_release_slot = 0xffffffff;
  IhsPointerEvent last_pointer{};
  uint32_t last_resize_w = 0;
  uint32_t last_resize_h = 0;

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

// Send a Hello with the given uuid/version onto `sock`.
bool SendHello(int sock, const uint8_t uuid[16], uint32_t version) {
  Hello h{};
  h.protocol_version = version;
  h.consumer_name_len = 0;
  std::memcpy(h.device_uuid, uuid, 16);
  return SendPod(sock, MsgType::kHello, h);
}
}  // namespace

static void HappyPath() {
  int sv[2];
  Check(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");

  MockSource src;
  Session session(sv[0], src);

  std::thread server([&] {
    if (session.Handshake()) {
      session.Run();
    }
  });

  // --- Consumer side on sv[1] ---
  Check(SendHello(sv[1], kUuid, kProtocolVersion), "send Hello");

  // Caps.
  RecvMsg caps_msg = RecvFramed(sv[1]);
  Check(caps_msg.ok && caps_msg.type == MsgType::kCaps, "recv Caps");
  Check(caps_msg.payload.size() == sizeof(Caps), "Caps size");
  if (caps_msg.payload.size() == sizeof(Caps)) {
    Caps caps{};
    std::memcpy(&caps, caps_msg.payload.data(), sizeof(caps));
    Check(caps.slot_count == kSlots, "Caps slot_count");
    Check(caps.protocol_version == kProtocolVersion, "Caps version");
    Check(caps.handle_types == kHandleOpaqueFd, "Caps handle_types mask");
    Check(std::memcmp(caps.device_uuid, kUuid, 16) == 0, "Caps uuid");
  }
  CloseFds(caps_msg);

  // Image table: header + 2 descs and 6 (=3*2) fds, each live/readable.
  RecvMsg table_msg = RecvFramed(sv[1]);
  Check(table_msg.ok && table_msg.type == MsgType::kImageTable,
        "recv ImageTable");
  Check(table_msg.fds.size() == ImageTableFdCount(kSlots),
        "ImageTable fd count == 6");
  Check(table_msg.payload.size() ==
            sizeof(ImageTableHeader) + kSlots * sizeof(ImageDesc),
        "ImageTable payload size");
  if (table_msg.payload.size() ==
      sizeof(ImageTableHeader) + kSlots * sizeof(ImageDesc)) {
    ImageTableHeader hdr{};
    std::memcpy(&hdr, table_msg.payload.data(), sizeof(hdr));
    Check(hdr.generation == 0, "ImageTable generation 0");
    Check(hdr.slot_count == kSlots, "ImageTable slot_count");
    ImageDesc desc0{};
    std::memcpy(&desc0, table_msg.payload.data() + sizeof(ImageTableHeader),
                sizeof(desc0));
    Check(desc0.width == kWidth && desc0.height == kHeight, "ImageDesc extent");
    Check(desc0.handle_type == kHandleOpaqueFd, "ImageDesc handle_type");
    Check(desc0.alloc_size == 0x100000, "ImageDesc alloc_size slot 0");
  }
  for (int fd : table_msg.fds) {
    char c = 0;
    Check(fd >= 0 && ::read(fd, &c, 1) == 1 && c == 'X', "table fd readable");
  }
  CloseFds(table_msg);

  // Present a frame from this (test) thread -> consumer should observe it.
  session.PresentFrame(/*slot=*/1, /*frame_seq=*/42);
  RecvMsg present_msg = RecvFramed(sv[1]);
  Check(present_msg.ok && present_msg.type == MsgType::kFramePresent,
        "recv FramePresent");
  if (present_msg.payload.size() == sizeof(FramePresent)) {
    FramePresent fp{};
    std::memcpy(&fp, present_msg.payload.data(), sizeof(fp));
    Check(fp.slot == 1 && fp.frame_seq == 42, "FramePresent slot/seq");
  }
  CloseFds(present_msg);

  // Release slot 1 -> the source's on_frame_release should fire.
  FrameRelease fr{/*slot=*/1, /*generation=*/0};
  Check(SendPod(sv[1], MsgType::kFrameRelease, fr), "send FrameRelease");

  // Bye ends Run().
  Bye bye{static_cast<uint32_t>(ByeReason::kNormal)};
  Check(SendPod(sv[1], MsgType::kBye, bye), "send Bye");

  server.join();
  Check(src.release_count == 1 && src.last_release_slot == 1,
        "on_frame_release saw slot 1");

  ::close(sv[0]);
  ::close(sv[1]);
}

static void DeviceMismatch() {
  int sv[2];
  Check(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0,
        "socketpair (mismatch)");

  MockSource src;
  Session session(sv[0], src);

  std::thread server([&] {
    // Handshake must reject; Run() should never start.
    if (session.Handshake()) {
      session.Run();
    }
  });

  uint8_t wrong[16];
  std::memcpy(wrong, kUuid, 16);
  wrong[0] ^= 0xff;  // corrupt the uuid.
  Check(SendHello(sv[1], wrong, kProtocolVersion), "send Hello (wrong uuid)");

  RecvMsg bye_msg = RecvFramed(sv[1]);
  Check(bye_msg.ok && bye_msg.type == MsgType::kBye, "recv Bye on mismatch");
  if (bye_msg.payload.size() == sizeof(Bye)) {
    Bye bye{};
    std::memcpy(&bye, bye_msg.payload.data(), sizeof(bye));
    Check(bye.reason == static_cast<uint32_t>(ByeReason::kDeviceMismatch),
          "Bye reason kDeviceMismatch");
  }
  CloseFds(bye_msg);

  server.join();
  Check(src.fill_count == 0, "no image table on mismatch");

  ::close(sv[0]);
  ::close(sv[1]);
}

int main() {
  HappyPath();
  DeviceMismatch();

  if (g_failures == 0) {
    std::printf("session_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "session_test: %d failure(s)\n", g_failures);
  return 1;
}
