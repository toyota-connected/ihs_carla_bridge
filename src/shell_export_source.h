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

// The production ExportSource: a thin adapter over the in-process shell C ABI
// (vk_export.h). It resolves `ihs_vk_export_get_api` via dlsym(RTLD_DEFAULT)
// (the ivi-homescreen headless-vulkan backend exports it at default
// visibility) and delegates every ExportSource seam to the V1 function table.
// Tests inject a fake get_api instead of touching dlsym. When no API is bound
// (`valid()` is false) every delegate is a safe no-op / default.

#ifndef IHS_CARLA_BRIDGE_SHELL_EXPORT_SOURCE_H_
#define IHS_CARLA_BRIDGE_SHELL_EXPORT_SOURCE_H_

#include "export_source.h"
#include "ihs_carla_bridge/vk_export.h"

namespace ihs_vke {

class ShellExportSource final : public ExportSource {
 public:
  // `get_api` is the entry point into the shell ABI. In production pass nullptr
  // and the shell symbol is resolved in-process with dlsym(RTLD_DEFAULT);
  // tests pass their own fake to avoid the process-global symbol.
  explicit ShellExportSource(IhsVkExportGetApiFn get_api = nullptr);

  // True once a V1 API table has been bound.
  bool valid() const { return api_ != nullptr; }

  // The bound V1 table (the Bridge uses it for set_frame_listener). May be
  // nullptr.
  const IhsVkExportApiV1* api() const { return api_; }

  // ExportSource seam -- each call delegates to the bound API, or returns a
  // safe default when none is bound.
  IhsVkExportCaps caps() override;
  bool fill_image_table(IhsVkExportImageTable* out) override;
  void on_frame_release(uint32_t slot) override;
  void on_input_pointer(const IhsPointerEvent& ev) override;
  bool on_resize(uint32_t width, uint32_t height) override;
  void on_consumer_attached() override;
  void on_consumer_detached() override;

 private:
  const IhsVkExportApiV1* api_ = nullptr;
};

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_SHELL_EXPORT_SOURCE_H_
