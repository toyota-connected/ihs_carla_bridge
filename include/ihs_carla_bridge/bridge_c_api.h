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

// The flat C ABI the ihs_carla_bridge shared module exports and the Dart
// package binds with dart:ffi. It is an opaque-handle wrapper over the C++
// Bridge coordinator: create a bridge on a socket path, start/stop its
// server + shell frame-listener wiring, then destroy it. These four functions
// are the module's only default-visibility exports.
//
// BridgeState reporting and NativePort event delivery are a later Dart-package
// step; this ABI is intentionally minimal.

#ifndef IHS_CARLA_BRIDGE_BRIDGE_C_API_H_
#define IHS_CARLA_BRIDGE_BRIDGE_C_API_H_

#define IHS_CB_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a bridge instance.
typedef struct IhsCarlaBridge IhsCarlaBridge;

// Create a bridge bound to `socket_path` (the AF_UNIX path the server will
// listen on). The shell export API is resolved in-process. Returns nullptr on
// a null path or allocation failure. The handle is not yet started.
IHS_CB_EXPORT IhsCarlaBridge* ihs_carla_bridge_create(const char* socket_path);

// Wire the shell frame listener and start the server. Returns 0 on success,
// -1 on failure (null handle, no shell API bound, or server start failure).
IHS_CB_EXPORT int ihs_carla_bridge_start(IhsCarlaBridge* bridge);

// Clear the shell frame listener and stop the server. Idempotent; null-safe.
IHS_CB_EXPORT void ihs_carla_bridge_stop(IhsCarlaBridge* bridge);

// Stop (if needed) and destroy the bridge. Null-safe.
IHS_CB_EXPORT void ihs_carla_bridge_destroy(IhsCarlaBridge* bridge);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IHS_CARLA_BRIDGE_BRIDGE_C_API_H_
