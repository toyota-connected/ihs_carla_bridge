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

#include "bridge.h"

#include <atomic>

#include "dart_api_dl.h"
#include "ihs_carla_bridge/bridge_c_api.h"

namespace ihs_vke {

namespace {
// Set once ihs_carla_bridge_init_dart_api succeeds; until then the Dart DL
// function pointers are null and posting would crash, so PostState checks this.
std::atomic<bool> g_dart_api_ready{false};
}  // namespace

void Bridge::FrameThunk(uint32_t slot, uint32_t frame_seq, void* user) {
  static_cast<Bridge*>(user)->server_.PresentFrame(slot, frame_seq);
}

void Bridge::PostState(int32_t code) {
  state_.store(code, std::memory_order_release);
  const int64_t port = event_port_.load(std::memory_order_acquire);
  if (port == 0 || !g_dart_api_ready.load(std::memory_order_acquire)) {
    return;
  }
  Dart_CObject obj;
  obj.type = Dart_CObject_kInt64;
  obj.value.as_int64 = code;
  Dart_PostCObject_DL(port, &obj);
}

void Bridge::SetEventPort(int64_t port) {
  event_port_.store(port, std::memory_order_release);
  if (port != 0) {
    // Seed the new listener with the current state.
    PostState(state_.load(std::memory_order_acquire));
  }
}

bool Bridge::Start() {
  if (!source_.valid()) {
    return false;
  }
  // Report consumer connect/disconnect from the Server's IO thread.
  server_.SetStateObserver([this](int32_t code) { PostState(code); });
  // Route the shell's raster-thread presents into the Server before it starts
  // accepting a consumer.
  source_.api()->set_frame_listener(&Bridge::FrameThunk, this);
  if (!server_.Start()) {
    source_.api()->set_frame_listener(nullptr, nullptr);
    return false;
  }
  PostState(IHS_CB_STATE_LISTENING);
  return true;
}

void Bridge::Stop() {
  if (source_.valid()) {
    source_.api()->set_frame_listener(nullptr, nullptr);
  }
  server_.Stop();
  PostState(IHS_CB_STATE_STOPPED);
}

}  // namespace ihs_vke

// ---- C ABI for dart:ffi (bridge_c_api.h) ------------------------------------
// Opaque-handle shim over ihs_vke::Bridge. Every entry point null-guards its
// handle. These four symbols are the ONLY default-visibility exports of the
// shared module.

using ihs_vke::Bridge;

extern "C" {

IhsCarlaBridge* ihs_carla_bridge_create(const char* socket_path) {
  if (socket_path == nullptr) {
    return nullptr;
  }
  Bridge* bridge = new Bridge(socket_path);
  return reinterpret_cast<IhsCarlaBridge*>(bridge);
}

int ihs_carla_bridge_start(IhsCarlaBridge* handle) {
  if (handle == nullptr) {
    return -1;
  }
  Bridge* bridge = reinterpret_cast<Bridge*>(handle);
  return bridge->Start() ? 0 : -1;
}

void ihs_carla_bridge_stop(IhsCarlaBridge* handle) {
  if (handle == nullptr) {
    return;
  }
  reinterpret_cast<Bridge*>(handle)->Stop();
}

void ihs_carla_bridge_destroy(IhsCarlaBridge* handle) {
  if (handle == nullptr) {
    return;
  }
  delete reinterpret_cast<Bridge*>(handle);
}

int ihs_carla_bridge_init_dart_api(void* data) {
  const intptr_t rc = Dart_InitializeApiDL(data);
  if (rc == 0) {
    ihs_vke::g_dart_api_ready.store(true, std::memory_order_release);
  }
  return static_cast<int>(rc);
}

void ihs_carla_bridge_set_event_port(IhsCarlaBridge* handle, int64_t port) {
  if (handle == nullptr) {
    return;
  }
  reinterpret_cast<Bridge*>(handle)->SetEventPort(port);
}

}  // extern "C"
