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

#include "shell_export_source.h"

#include <dlfcn.h>

namespace ihs_vke {

ShellExportSource::ShellExportSource(IhsVkExportGetApiFn get_api) {
  if (get_api == nullptr) {
    // Bind the shell symbol in-process; the headless-vulkan backend exports it
    // at default visibility. RTLD_DEFAULT searches the global scope so the
    // bridge never links the shell.
    get_api = reinterpret_cast<IhsVkExportGetApiFn>(
        ::dlsym(RTLD_DEFAULT, IHS_VK_EXPORT_GET_API_SYMBOL));
  }
  api_ = get_api ? get_api(IHS_VK_EXPORT_API_VERSION) : nullptr;
}

IhsVkExportCaps ShellExportSource::caps() {
  if (api_ == nullptr) {
    return IhsVkExportCaps{};
  }
  return api_->get_caps();
}

bool ShellExportSource::fill_image_table(IhsVkExportImageTable* out) {
  if (api_ == nullptr) {
    return false;
  }
  return api_->get_image_table(out) == 0;
}

void ShellExportSource::on_frame_release(uint32_t slot) {
  if (api_ != nullptr) {
    api_->release_frame(slot);
  }
}

void ShellExportSource::on_input_pointer(const IhsPointerEvent& ev) {
  if (api_ != nullptr) {
    api_->inject_pointer(&ev);
  }
}

bool ShellExportSource::on_resize(uint32_t width, uint32_t height) {
  if (api_ == nullptr) {
    return false;
  }
  return api_->rebuild_pool(width, height) == 0;
}

void ShellExportSource::on_consumer_attached() {
  if (api_ != nullptr) {
    api_->set_pace_source(IHS_VK_PACE_CONSUMER_DRIVEN);
  }
}

void ShellExportSource::on_consumer_detached() {
  if (api_ != nullptr) {
    api_->set_pace_source(IHS_VK_PACE_FREE_RUN);
  }
}

}  // namespace ihs_vke
