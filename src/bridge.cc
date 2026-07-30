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

#include "ihs_carla_bridge/bridge_c_api.h"

namespace ihs_vke {

void Bridge::FrameThunk(uint32_t slot, uint32_t frame_seq, void* user) {
  static_cast<Bridge*>(user)->server_.PresentFrame(slot, frame_seq);
}

bool Bridge::Start() {
  if (!source_.valid()) {
    return false;
  }
  // Route the shell's raster-thread presents into the Server before it starts
  // accepting a consumer.
  source_.api()->set_frame_listener(&Bridge::FrameThunk, this);
  if (!server_.Start()) {
    source_.api()->set_frame_listener(nullptr, nullptr);
    return false;
  }
  return true;
}

void Bridge::Stop() {
  if (source_.valid()) {
    source_.api()->set_frame_listener(nullptr, nullptr);
  }
  server_.Stop();
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

}  // extern "C"
