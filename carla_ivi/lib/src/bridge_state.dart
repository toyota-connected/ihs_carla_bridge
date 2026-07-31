// Copyright 2026 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// High-level state of a `CarlaBridge`, as reported by the native module.
///
/// A future revision streams transitions between these over a Dart native port;
/// the enum is the public vocabulary that stream (and any polling helper) uses.
enum BridgeState {
  /// Created but not started.
  idle,

  /// The server is started and listening for a consumer; none is connected.
  listening,

  /// A consumer completed the handshake and is receiving frames.
  consumerConnected,

  /// The server has stopped (after `stop()` or `dispose()`).
  stopped,
}
