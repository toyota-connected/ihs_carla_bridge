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

/// Dart FFI wrapper for the `ihs_carla_bridge` module.
///
/// The bridge is a native shared object loaded into the ivi-homescreen shell
/// process. It resolves the shell's exported `ihs_vk_export_get_api` symbol,
/// hands the headless-vulkan backend's exported frame pool to a CARLA consumer
/// over a Unix socket, and pumps input back. This package lets a Flutter/Dart
/// app drive that module's lifecycle (create / start / stop) instead of the
/// shell's env-gated native loader.
library;

import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'src/carla_ivi_bindings.g.dart';

export 'src/bridge_state.dart';

/// Default file name of the native module (resolved on the platform search
/// path unless an absolute path or a [DynamicLibrary] is supplied).
const String kDefaultLibraryName = 'libihs_carla_bridge.so';

/// Thrown when the native module cannot be loaded or a bridge cannot be created.
class CarlaBridgeException implements Exception {
  CarlaBridgeException(this.message);

  final String message;

  @override
  String toString() => 'CarlaBridgeException: $message';
}

/// A handle to one native `ihs_carla_bridge` instance.
///
/// Lifecycle: [CarlaBridge.open] creates it bound to a socket path, [start]
/// wires the shell frame listener and starts the server, [stop] tears the
/// server down (reversible), and [dispose] destroys it. The object must not be
/// used after [dispose].
class CarlaBridge {
  CarlaBridge._(this._bindings, this._handle, this.socketPath);

  final CarlaBridgeBindings _bindings;
  final String socketPath;
  Pointer<IhsCarlaBridge> _handle;

  /// Load the native module and create a bridge bound to [socketPath].
  ///
  /// Provide [library] to reuse an already-open [DynamicLibrary] (e.g. the
  /// running shell's own image via [DynamicLibrary.executable]/`process()`),
  /// or [libraryName] to override the module file name. Throws
  /// [CarlaBridgeException] if the module cannot be loaded or creation fails.
  factory CarlaBridge.open(
    String socketPath, {
    DynamicLibrary? library,
    String libraryName = kDefaultLibraryName,
  }) {
    final DynamicLibrary lib;
    try {
      lib = library ?? DynamicLibrary.open(libraryName);
    } on ArgumentError catch (e) {
      throw CarlaBridgeException('failed to load $libraryName: $e');
    }
    final bindings = CarlaBridgeBindings(lib);

    final cPath = socketPath.toNativeUtf8();
    try {
      final handle = bindings.ihs_carla_bridge_create(cPath.cast());
      if (handle == nullptr) {
        throw CarlaBridgeException(
          'ihs_carla_bridge_create returned null for "$socketPath"',
        );
      }
      return CarlaBridge._(bindings, handle, socketPath);
    } finally {
      malloc.free(cPath);
    }
  }

  /// True once [dispose] has run; every method below is then a no-op or throws.
  bool get isDisposed => _handle == nullptr;

  /// Wire the shell frame listener and start the server.
  ///
  /// Returns false when the native side reports failure — most commonly because
  /// the host is not the headless-vulkan shell (no export API resolved), or the
  /// socket could not be bound.
  bool start() {
    _ensureAlive();
    return _bindings.ihs_carla_bridge_start(_handle) == 0;
  }

  /// Clear the shell frame listener and stop the server. Idempotent; safe after
  /// [dispose].
  void stop() {
    if (isDisposed) {
      return;
    }
    _bindings.ihs_carla_bridge_stop(_handle);
  }

  /// Stop (if needed) and destroy the native bridge. Idempotent; the object is
  /// unusable afterward.
  void dispose() {
    if (isDisposed) {
      return;
    }
    _bindings.ihs_carla_bridge_destroy(_handle);
    _handle = nullptr;
  }

  void _ensureAlive() {
    if (isDisposed) {
      throw StateError('CarlaBridge used after dispose()');
    }
  }
}
