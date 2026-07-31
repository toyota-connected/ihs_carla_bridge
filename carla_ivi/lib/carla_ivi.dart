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

/// Dart FFI wrapper for the `ihs_carla_bridge` native module.
///
/// The bridge is a native shared object loaded into the ivi-homescreen shell
/// process. It resolves the shell's exported `ihs_vk_export_get_api` symbol,
/// hands the headless-vulkan backend's exported frame pool to a CARLA consumer
/// over a Unix socket, and pumps input back. This package lets a Flutter/Dart
/// app drive that module's lifecycle and observe its [BridgeState] instead of
/// the shell's env-gated native loader.
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import 'src/bridge_state.dart';
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
/// server down (reversible), and [dispose] destroys it. [state] is the last
/// known [BridgeState] and [states] streams transitions. The object must not be
/// used after [dispose].
class CarlaBridge {
  CarlaBridge._(this._bindings, this._handle, this.socketPath) {
    _controller = StreamController<BridgeState>.broadcast();
    _events = ReceivePort()..listen(_onNativeEvent);
  }

  final CarlaBridgeBindings _bindings;
  final String socketPath;
  Pointer<IhsCarlaBridge> _handle;

  late final StreamController<BridgeState> _controller;
  late final ReceivePort _events;
  BridgeState _state = BridgeState.idle;

  // The Dart DL API is process-global and only needs initializing once.
  static bool _dartApiReady = false;

  /// Load the native module and create a bridge bound to [socketPath].
  ///
  /// Provide [library] to reuse an already-open [DynamicLibrary] (e.g. the
  /// running shell's own image), or [libraryName] to override the module file
  /// name. Throws [CarlaBridgeException] if the module cannot be loaded or
  /// creation fails.
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

    // Initialize the Dart native API once so the module can post BridgeState
    // events to a native port. A failure here only disables the event stream.
    if (!_dartApiReady) {
      _dartApiReady = bindings
              .ihs_carla_bridge_init_dart_api(NativeApi.initializeApiDLData) ==
          0;
    }

    final cPath = socketPath.toNativeUtf8();
    final Pointer<IhsCarlaBridge> handle;
    try {
      handle = bindings.ihs_carla_bridge_create(cPath.cast());
    } finally {
      malloc.free(cPath);
    }
    if (handle == nullptr) {
      throw CarlaBridgeException(
        'ihs_carla_bridge_create returned null for "$socketPath"',
      );
    }

    final bridge = CarlaBridge._(bindings, handle, socketPath);
    // Register the native port; the module seeds the current state immediately.
    bindings.ihs_carla_bridge_set_event_port(
      handle,
      bridge._events.sendPort.nativePort,
    );
    return bridge;
  }

  /// True once [dispose] has run; every method below is then a no-op or throws.
  bool get isDisposed => _handle == nullptr;

  /// The most recent [BridgeState] reported by the module.
  BridgeState get state => _state;

  /// Transitions of [state]. Broadcast: late listeners miss earlier events but
  /// can read [state] for the current value.
  Stream<BridgeState> get states => _controller.stream;

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
    // Unregister the port before destroying so no event races the teardown.
    _bindings.ihs_carla_bridge_set_event_port(_handle, 0);
    _bindings.ihs_carla_bridge_destroy(_handle);
    _handle = nullptr;
    _events.close();
    if (!_controller.isClosed) {
      _controller.close();
    }
  }

  void _onNativeEvent(Object? message) {
    if (message is! int ||
        message < 0 ||
        message >= BridgeState.values.length) {
      return;
    }
    _state = BridgeState.values[message];
    if (!_controller.isClosed) {
      _controller.add(_state);
    }
  }

  void _ensureAlive() {
    if (isDisposed) {
      throw StateError('CarlaBridge used after dispose()');
    }
  }
}
