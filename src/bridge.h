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

// The module coordinator: it owns the production ShellExportSource and the
// Server, and wires the shell's per-present frame listener to the Server so
// raster-thread presents reach the connected consumer. This is the top of the
// C++ stack; the dart:ffi C ABI (bridge_c_api.h) is a thin opaque-handle shim
// over it.

#ifndef IHS_CARLA_BRIDGE_BRIDGE_H_
#define IHS_CARLA_BRIDGE_BRIDGE_H_

#include <cstdint>
#include <string>
#include <utility>

#include "ihs_carla_bridge/vk_export.h"
#include "server.h"
#include "shell_export_source.h"

namespace ihs_vke {

class Bridge {
 public:
  // `socket_path` is the AF_UNIX path the Server binds. `get_api` is forwarded
  // to the ShellExportSource: nullptr binds the in-process shell symbol; tests
  // inject a fake.
  Bridge(std::string socket_path, IhsVkExportGetApiFn get_api = nullptr)
      : source_(get_api), server_(std::move(socket_path), source_) {}

  ~Bridge() { Stop(); }

  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;

  // True if a shell API was bound (the backend is headless-vulkan).
  bool valid() const { return source_.valid(); }

  // Register the shell frame listener and start the Server. Returns false if
  // no API is bound or the Server fails to start (in which case the listener
  // is cleared).
  bool Start();

  // Clear the shell frame listener and stop the Server. Idempotent.
  void Stop();

 private:
  // Shell raster-thread present callback: forwards to the Server, which is
  // thread-safe. `user` is the owning Bridge.
  static void FrameThunk(uint32_t slot, uint32_t frame_seq, void* user);

  ShellExportSource source_;
  Server server_;
};

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_BRIDGE_H_
