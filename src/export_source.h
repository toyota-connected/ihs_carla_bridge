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

// The abstract source a Session pulls the exported image pool from and pushes
// consumer events into. In production this wraps `ihs_vk_export_get_api` (the
// in-process shell C ABI, see vk_export.h); tests substitute a mock. Every
// method is called on the Session's own thread (never concurrently), so
// implementations need no internal locking on this seam.

#ifndef IHS_CARLA_BRIDGE_EXPORT_SOURCE_H_
#define IHS_CARLA_BRIDGE_EXPORT_SOURCE_H_

#include <cstdint>

#include "ihs_carla_bridge/vk_export.h"

namespace ihs_vke {

class ExportSource {
 public:
  virtual ~ExportSource() = default;

  // Pool caps (device_uuid, handle_type, format, extent, slot_count).
  virtual IhsVkExportCaps caps() = 0;

  // Fill the current table with descriptors + DUP'd fds the SESSION now owns
  // and must close after sending. Return false if there is no exportable pool.
  virtual bool fill_image_table(IhsVkExportImageTable* out) = 0;

  // Consumer released a slot -> the pacing edge (forward to the backend).
  virtual void on_frame_release(uint32_t slot) = 0;

  // Consumer pointer input (forward through the engine's inject path).
  virtual void on_input_pointer(const IhsPointerEvent& ev) = 0;

  // Rebuild the pool at a new extent. Return true if a new table is available.
  virtual bool on_resize(uint32_t width, uint32_t height) = 0;
};

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_EXPORT_SOURCE_H_
