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

// Smoke test for the FFI lifecycle wiring. Requires a built
// libihs_carla_bridge.so; point IHS_CB_LIB at it, or it is resolved from the
// repo's default host build dir. The test skips (does not fail) when absent.
//
// The bridge's start() is expected to fail here: a plain Dart process is not
// the headless-vulkan shell, so ihs_vk_export_get_api resolves to nothing and
// the module reports no export API. The point is to prove create/stop/dispose
// cross the FFI boundary cleanly and that lifecycle guards hold.

import 'dart:io';

import 'package:carla_ivi/carla_ivi.dart';
import 'package:test/test.dart';

String? _resolveLibrary() {
  final env = Platform.environment['IHS_CB_LIB'];
  if (env != null && File(env).existsSync()) {
    return env;
  }
  // Default: the host build tree one level up from this package.
  final scriptDir = File.fromUri(Platform.script).parent.path;
  for (final candidate in <String>[
    '$scriptDir/../../build/libihs_carla_bridge.so',
    '${Directory.current.path}/../build/libihs_carla_bridge.so',
    '${Directory.current.path}/build/libihs_carla_bridge.so',
  ]) {
    if (File(candidate).existsSync()) {
      return candidate;
    }
  }
  return null;
}

void main() {
  final libPath = _resolveLibrary();

  group('CarlaBridge FFI lifecycle', () {
    test(
      'create/start/stop/dispose cross the boundary',
      () {
        final socket = '${Directory.systemTemp.path}/carla_ivi_test.sock';
        final bridge = CarlaBridge.open(socket, libraryName: libPath!);

        expect(bridge.isDisposed, isFalse);
        expect(bridge.socketPath, socket);

        // No shell export API in this process, so start() reports failure but
        // must not crash.
        expect(bridge.start(), isFalse);

        bridge.stop(); // idempotent, no-op friendly
        bridge.dispose();
        expect(bridge.isDisposed, isTrue);

        // Post-dispose: stop() is a safe no-op, start() throws.
        bridge.stop();
        expect(bridge.start, throwsStateError);
        bridge.dispose(); // idempotent
      },
      skip: libPath == null
          ? 'libihs_carla_bridge.so not found (set IHS_CB_LIB)'
          : null,
    );

    test(
      'state transitions reach the Dart stream',
      () async {
        final socket = '${Directory.systemTemp.path}/carla_ivi_events.sock';
        final bridge = CarlaBridge.open(socket, libraryName: libPath!);

        // stop() posts a STOPPED transition over the native port even when the
        // server never started; subscribe first, then trigger it. This proves
        // init_dart_api / set_event_port / Dart_PostCObject / ReceivePort all
        // work, without needing the headless-vulkan shell.
        final stopped = bridge.states
            .firstWhere((s) => s == BridgeState.stopped)
            .timeout(const Duration(seconds: 3));
        bridge.stop();
        expect(await stopped, BridgeState.stopped);
        expect(bridge.state, BridgeState.stopped);

        bridge.dispose();
      },
      skip: libPath == null
          ? 'libihs_carla_bridge.so not found (set IHS_CB_LIB)'
          : null,
    );

    test('open throws for a missing library', () {
      expect(
        () => CarlaBridge.open('/tmp/x.sock',
            libraryName: '/nonexistent/libihs_carla_bridge.so'),
        throwsA(isA<CarlaBridgeException>()),
      );
    });
  });
}
